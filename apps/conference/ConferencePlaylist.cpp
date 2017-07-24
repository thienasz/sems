#include "AmPlaylist.h"


int ConferencePlaylist::get(unsigned long long system_ts, unsigned char* buffer, 
		    int output_sample_rate, unsigned int nb_samples)
{
  int ret = 0;

  if(get_company_channel) {
    company_mut.lock();
    DBG("get company");
    ret = company_item->play->get(system_ts,buffer,
           output_sample_rate,
           nb_samples);
    company_mut.unlock();
    return ret;
  }

if(cur_item && cur_item->play){
  cur_mut.lock();
  DBG("get group\n");
  ret = cur_item->play->get(system_ts,buffer,
                                     output_sample_rate,
                                     nb_samples);
  cur_mut.unlock();
}else {
 // ret = calcBytesToRead(nb_samples);
 // DBG("memset buffer");
 // memset(buffer,0,ret);
}

  return ret;
}

int ConferencePlaylist::put(unsigned long long system_ts, unsigned char* buffer, 
		    int input_sample_rate, unsigned int size)
{
  int ret = 0;

  if(put_company_channel) {
    DBG("put to company channel\n");
    company_mut.lock();

    ret = company_item->record->put(system_ts,buffer,
    				 input_sample_rate,
    				 size);

    company_mut.unlock();
    	
    return ret;
  }

if(put_group_channel){ 
  bool hasRecordFlag = false;
  sub_items_mut.lock();
  for (map<string, AmPlaylistItem*>::iterator it=sub_items.begin(); it!=sub_items.end(); it++) {
      if(it->second->record) {
      hasRecordFlag = true;
      DBG("put to room: %s \n", it->first.c_str());
      ret = it->second->record->put(system_ts,buffer,
				     input_sample_rate,
				     size);
	}
  }

  if(sub_items.empty() || !hasRecordFlag) {
      ret = size;
  }
  sub_items_mut.unlock();
}

  return ret;
}

ConferencePlaylist::ConferencePlaylist(AmEventQueue* q)
  : AmAudio(new AmAudioFormat(CODEC_PCM16)),
    ev_q(q), cur_item(0)
{
  
}

ConferencePlaylist::~ConferencePlaylist() {
  flush();
}

void ConferencePlaylist::addCompanyToPlaylist(AmPlaylistItem* item)
{
  if(company_item)
    return;
  DBG("vao add company");
  company_mut.lock();
  company_item = item;
  company_mut.unlock();
}

void ConferencePlaylist::addToSubPlaylist(string conf, AmPlaylistItem* item)
{
  sub_items_mut.lock();
  DBG("enter add back size sub_items: %zd\n", sub_items.size());

  sub_items.insert(std::pair<string,AmPlaylistItem*>(conf,item));
  DBG("end add front size sub_items: %zd\n", sub_items.size());

  sub_items_mut.unlock();
}

void ConferencePlaylist::PutToGroupChannel(bool is_put)
{
  DBG("PutToGroupChannel chl: %s\n", activeChannel.c_str());
  put_channel_mut.lock();
  put_group_channel = is_put;
  put_channel_mut.unlock();
}

void ConferencePlaylist::PutToCompanyChannel(bool is_put)
{
  DBG(" PutToCompanyChannel\n");
  put_channel_mut.lock();
  put_company_channel = is_put;
  put_channel_mut.unlock();
}

void ConferencePlaylist::setActiveGetCompanyChannel(bool is_get)
{
  DBG("setActiveGetCompanyChannel\n");
  get_company_channel_mut.lock();
  get_company_channel = is_get;
  get_company_channel_mut.unlock();
}

void ConferencePlaylist::setActiveGetChannel(string channel)
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


void ConferencePlaylist::setDeactiveGetChannel(string channel)
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

void ConferencePlaylist::flushChannel()
{
  DBG("flushChannel\n");

  company_mut.lock();
  delete company_item;
  company_mut.unlock();

  sub_items_mut.lock();
  sub_items.clear();
  sub_items_mut.unlock();
}

