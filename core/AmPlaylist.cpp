/*
 * Copyright (C) 2002-2003 Fhg Fokus
 *
 * This file is part of SEMS, a free SIP media server.
 *
 * SEMS is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version. This program is released under
 * the GPL with the additional exemption that compiling, linking,
 * and/or using OpenSSL is allowed.
 *
 * For a license to use the SEMS software under conditions
 * other than those described here, or to purchase support for this
 * software, please contact iptel.org by e-mail at the following addresses:
 *    info@iptel.org
 *
 * SEMS is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License 
 * along with this program; if not, write to the Free Software 
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "AmPlaylist.h"
#include "amci/codecs.h"
#include "log.h"

void AmPlaylist::updateCurrentItem()
{
  if(!cur_item){
    items_mut.lock();
    if(!items.empty()){
      cur_item = items.front();
      items.pop_front();
    }
    items_mut.unlock();
  }
}

void AmPlaylist::gotoNextItem(bool notify)
{
  bool had_item = false;
  if(cur_item){

    delete cur_item;
    cur_item = 0;
    had_item = true;
  }

  updateCurrentItem();
  if(notify && had_item && !cur_item && ev_q){
    DBG("posting AmAudioEvent::noAudio event!\n");
    ev_q->postEvent(new AmAudioEvent(AmAudioEvent::noAudio));
  }
}

int AmPlaylist::get(unsigned long long system_ts, unsigned char* buffer, 
		    int output_sample_rate, unsigned int nb_samples)
{
  int ret = -1;

  cur_mut.lock();
  updateCurrentItem();

  while(cur_item && 
	cur_item->play && 
	(ret = cur_item->play->get(system_ts,buffer,
				   output_sample_rate,
				   nb_samples)) <= 0) {

    DBG("get: gotoNextItem\n");
    gotoNextItem(true);
  }

  if(!cur_item || !cur_item->play) {
    ret = calcBytesToRead(nb_samples);
    memset(buffer,0,ret);
  }

  cur_mut.unlock();
  return ret;
}

int AmPlaylist::put(unsigned long long system_ts, unsigned char* buffer, 
		    int input_sample_rate, unsigned int size)
{
  int ret = -1;

  cur_mut.lock();
  updateCurrentItem();
  while(cur_item && 
	cur_item->record &&
	(ret = cur_item->record->put(system_ts,buffer,
				     input_sample_rate,
				     size)) < 0) {

    DBG("put: gotoNextItem\n");
    gotoNextItem(true);
  }

  if(!cur_item || !cur_item->record)
    ret = size;
    
  cur_mut.unlock();
  return ret;
}

AmPlaylist::AmPlaylist(AmEventQueue* q)
  : AmAudio(new AmAudioFormat(CODEC_PCM16)),
    ev_q(q), cur_item(0)
{
  
}

AmPlaylist::~AmPlaylist() {
  flush();
}

void AmPlaylist::addToPlaylist(AmPlaylistItem* item)
{
  items_mut.lock();
  items.push_back(item);
  items_mut.unlock();
}

void AmPlaylist::addToPlayListFront(AmPlaylistItem* item)
{
  cur_mut.lock();
  items_mut.lock();
  if(cur_item){
    items.push_front(cur_item);
    cur_item = item;
  }
  else {
    items.push_front(item);
  }    
  items_mut.unlock();
  cur_mut.unlock();
}

void AmPlaylist::close()
{
  DBG("flushing playlist before closing\n");
  flush();
  AmAudio::close();
}

void AmPlaylist::flush()
{
  cur_mut.lock();
  if(!cur_item && !items.empty()){
    cur_item = items.front();
    items.pop_front();
  }

  while(cur_item)
    gotoNextItem(false);
  cur_mut.unlock();
}

bool AmPlaylist::isEmpty()
{
  bool res(true);

  cur_mut.lock();
  items_mut.lock();

  res = (!cur_item) && items.empty();
    
  items_mut.unlock();
  cur_mut.unlock();

  return res;
}

void AmPlaylist::addCompanyToPlaylist(AmPlaylistItem* item)
{
  if(company_item)
    return;
  DBG("vao add company");
  company_mut.lock();
  company_item = item;
  company_mut.unlock();
}

void AmPlaylist::addToSubPlaylist(string conf, AmPlaylistItem* item)
{
  sub_items_mut.lock();
  DBG("enter add back size sub_items: %zd\n", sub_items.size());

  sub_items.insert(std::pair<string,AmPlaylistItem*>(conf,item));
  DBG("end add front size sub_items: %zd\n", sub_items.size());

  sub_items_mut.unlock();
}

void AmPlaylist::PutToGroupChannel(bool is_put)
{
  DBG("PutToGroupChannel chl: %s\n", activeChannel.c_str());
  put_channel_mut.lock();
  put_group_channel = is_put;
  put_channel_mut.unlock();
}

void AmPlaylist::PutToCompanyChannel(bool is_put)
{
  DBG(" PutToCompanyChannel\n");
  put_channel_mut.lock();
  put_company_channel = is_put;
  put_channel_mut.unlock();
}

void AmPlaylist::setActiveGetCompanyChannel(bool is_get)
{
  DBG("setActiveGetCompanyChannel\n");
  get_company_channel_mut.lock();
  get_company_channel = is_get;
  get_company_channel_mut.unlock();
}

void AmPlaylist::setActiveGetChannel(string channel)
{
  DBG("setActiveGetChannel: %s\n", channel.c_str());
  map<string, AmPlaylistItem*>::iterator it = sub_items.find(channel);
  items_mut.lock();
  if(it != sub_items.end()) {
    activeChannel = it->first;
    cur_item = it->second;
    DBG("set ok\n");
  }
  items_mut.unlock();
}


void AmPlaylist::setDeactiveGetChannel(string channel)
{
  DBG("setDeactiveGetChannel: %s\n", channel.c_str());
  map<string, AmPlaylistItem*>::iterator it = sub_items.find(channel);
  if(it->second == cur_item) {
    DBG("setDeactiveGetChannel ok: %s\n", channel.c_str());
    items_mut.lock();
    cur_item = NULL;
    items_mut.unlock();
  }
}

void AmPlaylist::flushChannel()
{
  DBG("flushChannel\n");

  company_mut.lock();
  delete company_item;
  company_mut.unlock();

  sub_items_mut.lock();
  sub_items.clear();
  sub_items_mut.unlock();
}

