/*
 * Copyright (C) 2002-2003 Fhg Fokus
 * Copyright (C) 2007 Juha Heinanen (USE_MYSQL parts)
 *
 * This file is part of SEMS, a free SIP media server.
 *
 * SEMS is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * For a license to use the sems software under conditions
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

#include "Conference.h"
#include "AmUtils.h"
#include "AmConfigReader.h"
#include "AmConferenceStatus.h"
#include "AmConfig.h"

#include "AmSessionContainer.h"
#include "AmMediaProcessor.h"
#include "ampi/MonitoringAPI.h"

#include "sems.h"
#include "log.h"

#ifdef USE_MYSQL
#include <mysql++/mysql++.h>
#include <stdio.h>
#define DEFAULT_AUDIO_TABLE "default_audio"
#define DOMAIN_AUDIO_TABLE "domain_audio"
#define LONELY_USER_MSG "first_participant_msg"
#define JOIN_SOUND "join_snd"
#define DROP_SOUND "drop_snd"
#endif

#define APP_NAME "conference"
	
EXPORT_SESSION_FACTORY(ConferenceFactory,APP_NAME);


#ifdef WITH_SAS_TTS
#define TTS_CACHE_PATH "/tmp/"
extern "C" cst_voice *register_cmu_us_kal();
#endif

ConferenceFactory::ConferenceFactory(const string& _app_name)
  : AmSessionFactory(_app_name)
{
}

std::multimap<string, ConferenceDialog*> ConferenceFactory::Comp2Session;
std::multimap<string, ConferenceDialog*> ConferenceFactory::Conf2Session;

string ConferenceFactory::AudioPath;
string ConferenceFactory::LonelyUserFile;
string ConferenceFactory::JoinSound;
string ConferenceFactory::DropSound;
string ConferenceFactory::DialoutSuffix;
PlayoutType ConferenceFactory::m_PlayoutType = ADAPTIVE_PLAYOUT;
unsigned int ConferenceFactory::MaxParticipants;

bool ConferenceFactory::UseRFC4240Rooms;
AmConfigReader ConferenceFactory::cfg;
AmSessionEventHandlerFactory* ConferenceFactory::session_timer_f = NULL;

#ifdef USE_MYSQL
mysqlpp::Connection ConferenceFactory::Connection(mysqlpp::use_exceptions);

int get_audio_file(const string& message, const string& domain, const string& language,
		   string& audio_file)
{
  string query_string;

  if (language.empty()) {
    if (domain.empty()) {
      audio_file = string("/tmp/") + APP_NAME + "_" + message + ".wav";
      query_string = "select audio from " + string(DEFAULT_AUDIO_TABLE) + " where application='" + APP_NAME + "' and message='" + message + "' and language=''";
    } else {
      audio_file = "/tmp/" + domain + "_" + APP_NAME + "_" + 
	message + ".wav";
      query_string = "select audio from " + string(DOMAIN_AUDIO_TABLE) + " where application='" + APP_NAME + "' and message='" + message + "' and domain='" + domain + "' and language=''";
    }
  } else {
    if (domain.empty()) {
      audio_file = "/tmp/" APP_NAME  "_" + message + "_" +
	language + ".wav";
      query_string = "select audio from " + string(DEFAULT_AUDIO_TABLE) + " where application='" + APP_NAME + "' and message='" + message + "' and language='" + language + "'";
    } else {
      audio_file = "/tmp/" + domain + "_"  APP_NAME  "_" +
	message + "_" +	language + ".wav";
      query_string = "select audio from " + string(DOMAIN_AUDIO_TABLE) + " where application='" + APP_NAME + "' and message='" + message + "' and domain='" + domain + "' and language='" + language + "'";
    }
  }

  try {

    mysqlpp::Query query = ConferenceFactory::Connection.query();
	    
    DBG("Query string <%s>\n", query_string.c_str());

    query << query_string;

#ifdef VERSION2
    mysqlpp::Result res = query.store();
#else
    mysqlpp::StoreQueryResult res = query.store();
#endif

    mysqlpp::Row row;

    if (res) {
      if ((res.num_rows() > 0) && (row = res.at(0))) {
	FILE *file;
	file = fopen(audio_file.c_str(), "wb");
#ifdef VERSION2
	unsigned long length = row.raw_string(0).size();
	fwrite(row.at(0).data(), 1, length, file);
#else
	mysqlpp::String s = row[0];
	fwrite(s.data(), 1, s.length(), file);
#endif
	fclose(file);
	return 1;
      } else {
	audio_file = "";
	return 1;
      }
    } else {
      ERROR("Database query error\n");
      audio_file = "";
      return 0;
    }
  }

  catch (const mysqlpp::Exception& er) {
    // Catch-all for any MySQL++ exceptions
    ERROR("MySQL++ error: %s\n", er.what());
    audio_file = "";
    return 0;
  }
}

#endif

int ConferenceFactory::onLoad()
{
  if(cfg.loadFile(AmConfig::ModConfigPath + string(APP_NAME)+ ".conf")) {
    ERROR("Configuration file '%s' missing.\n",
	  (AmConfig::ModConfigPath + string(APP_NAME)+ ".conf").c_str());
    return -1;
  }

  // get application specific global parameters
  configureModule(cfg);

#ifdef USE_MYSQL

  /* Get default audio from MySQL */

  string mysql_server, mysql_user, mysql_passwd, mysql_db, mysql_ca_cert;

  mysql_server = cfg.getParameter("mysql_server");
  if (mysql_server.empty()) {
    mysql_server = "localhost";
  }

  mysql_user = cfg.getParameter("mysql_user");
  if (mysql_user.empty()) {
    ERROR("conference.conf parameter 'mysql_user' is missing.\n");
    return -1;
  }

  mysql_passwd = cfg.getParameter("mysql_passwd");
  if (mysql_passwd.empty()) {
    ERROR("conference.conf parameter 'mysql_passwd' is missing.\n");
    return -1;
  }

  mysql_db = cfg.getParameter("mysql_db");
  if (mysql_db.empty()) {
    mysql_db = "sems";
  }

  mysql_ca_cert = cfg.getParameter("mysql_ca_cert");

  try {

#ifdef VERSION2
    Connection.set_option(Connection.opt_reconnect, true);
#else
    Connection.set_option(new mysqlpp::ReconnectOption(true));
#endif
    if (!mysql_ca_cert.empty())
      Connection.set_option(
	new mysqlpp::SslOption(0, 0, mysql_ca_cert.c_str(), "",
			       "DHE-RSA-AES256-SHA"));
    Connection.connect(mysql_db.c_str(), mysql_server.c_str(),
		       mysql_user.c_str(), mysql_passwd.c_str());
    if (!Connection) {
      ERROR("Database connection failed: %s\n", Connection.error());
      return -1;
    }
  }

  catch (const mysqlpp::BadOption& er) {
    ERROR("MySQL++ set_option error: %s\n", er.what());
    return -1;
  }

  catch (const mysqlpp::Exception& er) {
    // Catch-all for any MySQL++ exceptions
    ERROR("MySQL++ error: %s\n", er.what());
    return -1;
  }

  if (!get_audio_file(LONELY_USER_MSG, "", "", LonelyUserFile)) {
    return -1;
  }

  if (LonelyUserFile.empty()) {
    ERROR("default announce 'first_participant_msg'\n");
    ERROR("for module conference does not exist.\n");
    return -1;
  }

  if (!get_audio_file(JOIN_SOUND, "", "", JoinSound)) {
    return -1;
  }

  if (!get_audio_file(DROP_SOUND, "", "", DropSound)) {
    return -1;
  }

#else 

  /* Get default audio from file system */

  AudioPath = cfg.getParameter("audio_path", ANNOUNCE_PATH);

  LonelyUserFile = cfg.getParameter("default_announce");
  if (LonelyUserFile.empty()) {
    LonelyUserFile = AudioPath + "/" ANNOUNCE_FILE;
  } else {
    if (LonelyUserFile[0] != '/') {
      LonelyUserFile = AudioPath + "/" + LonelyUserFile;
    }
  }
  if(!file_exists(LonelyUserFile)){
    ERROR("default announce '%s' \n",LonelyUserFile.c_str());
    ERROR("for module conference does not exist.\n");
    return -1;
  }

  JoinSound = cfg.getParameter("join_sound");
  if (!JoinSound.empty()) {
    if (JoinSound[0] != '/') {
      JoinSound = AudioPath + "/" + JoinSound;
    }
  }

  DropSound = cfg.getParameter("drop_sound");
  if (!DropSound.empty()) {
    if (DropSound[0] != '/') {
      DropSound = AudioPath + "/" + DropSound;
    }
  }

#endif
	
  DialoutSuffix = cfg.getParameter("dialout_suffix");
  if(DialoutSuffix.empty()){
    WARN("No dialout_suffix has been configured in the conference plug-in:\n");
    WARN("\t -> dial out will not be available unless P-Dialout-Suffix\n");
    WARN("\t -> header parameter is passed to conference plug-in\n");
  }
    
  string playout_type = cfg.getParameter("playout_type");
  if (playout_type == "simple") {
    m_PlayoutType = SIMPLE_PLAYOUT;
    DBG("Using simple (fifo) buffer as playout technique.\n");
  } else 	if (playout_type == "adaptive_jb") {
    m_PlayoutType = JB_PLAYOUT;
    DBG("Using adaptive jitter buffer as playout technique.\n");
  } else {
    DBG("Using adaptive playout buffer as playout technique.\n");
  }

  MaxParticipants = 0;
  string max_participants = cfg.getParameter("max_participants");
  if (max_participants.length() && str2i(max_participants, MaxParticipants)) {
    ERROR("while parsing max_participants parameter\n"); 
  }

  UseRFC4240Rooms = cfg.getParameter("use_rfc4240_rooms")=="yes";
  DBG("%ssing RFC4240 room naming.\n", UseRFC4240Rooms?"U":"Not u");

  if(cfg.hasParameter("enable_session_timer") &&
     (cfg.getParameter("enable_session_timer") == string("yes")) ){
    DBG("enabling session timers\n");
    session_timer_f = AmPlugIn::instance()->getFactory4Seh("session_timer");
    if(session_timer_f == NULL){
      ERROR("Could not load the session_timer module: disabling session timers.\n");
    }
  }

  return 0;
}

AmSession* ConferenceFactory::onInvite(const AmSipRequest& req, const string& app_name,
				       const map<string,string>& app_params)
{
  if ((ConferenceFactory::MaxParticipants > 0) &&
      (AmConferenceStatus::getConferenceSize(req.user) >=
      ConferenceFactory::MaxParticipants)) {
    DBG("Conference is full.\n");
    throw AmSession::Exception(486, "Busy Here");
  }

  DBG("factory invite \n"); 

  string full_conf_ids = req.user.substr(4);
  string company_id= full_conf_ids.substr(0, full_conf_ids.find("*")+1);
  string conf_ids = full_conf_ids.substr(full_conf_ids.find("*")+1);
  DBG("conf: %s - company: %s\n", conf_ids.c_str(), company_id.c_str());
  full_conf_ids = req.user;

  if (UseRFC4240Rooms) {
    // see RFC4240 5.  Conference Service
    if (req.user.length()<5)
      throw AmSession::Exception(404, "Not Found");
    
    if (req.user.substr(0,5)!="conf=")
      throw AmSession::Exception(404, "Not Found");

    conf_ids = req.user.substr(5);
  }

  ConferenceDialog* s = new ConferenceDialog(full_conf_ids);
  DBG("new conference dialog: id = %s\n",conf_ids.c_str());
  setupSessionTimer(s);

  //Comp2Session.insert(std::make_pair(company_id, s));

  //s->addSubConf(conf_ids);
#if 1
  string conf_id = "";

  //s->addSubConf(company_id);
  for (std::string::iterator it=conf_ids.begin(); it!=conf_ids.end(); ++it){
    conf_id += *it;
	if(conf_id.length() == 3) {
	  DBG("sub conf: %s - company: %s\n", conf_id.c_str(), company_id.c_str());
      s->addSubConf(company_id + conf_id);
	  //Conf2Session.insert(std::make_pair(company_id + conf_id, s));
	  conf_id = "";
	}
  }

#endif
  s->setCompanyId(company_id);
  return s;
}

void ConferenceFactory::setupSessionTimer(AmSession* s) {
  if (NULL != session_timer_f) {

    AmSessionEventHandler* h = session_timer_f->getHandler(s);
    if (NULL == h)
      return;

    if(h->configure(cfg)){
      ERROR("Could not configure the session timer: disabling session timers.\n");
      delete h;
    } else {
      s->addHandler(h);
    }
  }
}

void ConferenceFactory::connectToCompany(ConferenceDialog* conferenceActive){
  DBG("enter connect all\n");

}

void ConferenceFactory::cancelConnectCompany(ConferenceDialog* conferenceActive){
  DBG("enter connect cancelConnectAll\n");

}

void ConferenceFactory::connectToGroup(ConferenceDialog* conferenceActive){
  DBG("enter connect connectToGroup\n");
  
}

void ConferenceFactory::cancelConnectGroup(ConferenceDialog* conferenceActive){
  DBG("enter connect cancelConnectGroup\n");

}

void ConferenceFactory::connectToAll(ConferenceDialog* conferenceActive){
  DBG("enter connect all\n");

}

void ConferenceFactory::cancelConnectAll(ConferenceDialog* conferenceActive){
  DBG("enter connect cancelConnectAll\n");

}

AmSession* ConferenceFactory::onRefer(const AmSipRequest& req, const string& app_name,
				      const map<string,string>& app_params)
{
  if(req.to_tag.empty())
    throw AmSession::Exception(488,"Not accepted here");

  AmSession* s = new ConferenceDialog(req.user);
  s->dlg->setLocalTag(req.from_tag);
  
  setupSessionTimer(s);

  DBG("ConferenceFactory::onRefer: local_tag = %s\n",s->dlg->getLocalTag().c_str());

  return s;
}

ConferenceDialog::ConferenceDialog(const string& conf_id,
				   AmConferenceChannel* dialout_channel)
  : conf_id(conf_id), 
    channel(0),
    play_list(this),
    dialout_channel(dialout_channel),
    state(CS_normal),
    allow_dialout(false),
    isGroupPtt(false),
    rtp_status(RTP_unknown),
    rtp_recv(RTP_c_unknown),
    ptt_status(PTT_unkonw),
    active_room("")
{
  dialedout = this->dialout_channel.get() != 0;
  RTPStream()->setPlayoutType(ConferenceFactory::m_PlayoutType);
#ifdef WITH_SAS_TTS
  tts_voice = register_cmu_us_kal();
#endif  
}

ConferenceDialog::~ConferenceDialog()
{
  DBG("ConferenceDialog::~ConferenceDialog()\n");

  if(ptt_status == PTT_company) {
  	DBG("ConferenceDialog::~ConferenceDialog() cancel connect company\n");
    cancelConnectCompany();
  } else if(ptt_status == PTT_group) {
  	DBG("ConferenceDialog::~ConferenceDialog() cancel connect group\n");
    cancelConnectGroup();
  }

  // clean playlist items
  play_list.flush();
  play_list.flushChannel();
#ifdef WITH_SAS_TTS
  // garbage collect tts files - TODO: delete files
  for (vector<AmAudioFile*>::iterator it =
	 TTSFiles.begin();it!=TTSFiles.end();it++) {
    delete *it;
  }
#endif
}

void ConferenceDialog::onStart() {
}

void ConferenceDialog::onInvite(const AmSipRequest& req)
{
  if(dlg->getStatus() == AmSipDialog::Connected){
    AmSession::onInvite(req);  
    return;
  }

  DBG("oninvite conference dialog\n");
  int i, len;
  string lonely_user_file;

  string app_param_hdr = getHeader(req.hdrs, PARAM_HDR, true);
  string listen_only_str = "";
  if (app_param_hdr.length()) {
    from_header = get_header_keyvalue(app_param_hdr, "Dialout-From");
    extra_headers = get_header_keyvalue(app_param_hdr, "Dialout-Extra");
    dialout_suffix = get_header_keyvalue(app_param_hdr, "Dialout-Suffix");      
    language = get_header_keyvalue(app_param_hdr, "Language");
    listen_only_str = get_header_keyvalue(app_param_hdr, "Listen-Only");
  } else {
    from_header = getHeader(req.hdrs, "P-Dialout-From", true);
    extra_headers = getHeader(req.hdrs, "P-Dialout-Extra", true);
    dialout_suffix = getHeader(req.hdrs, "P-Dialout-Suffix", true);
    if (from_header.length() || extra_headers.length() 
	|| dialout_suffix.length()) {
      DBG("Warning: P-Dialout- style headers are deprecated."
	  " Please use P-App-Param header instead.\n");
    }
    language = getHeader(req.hdrs, "P-Language", true);
    if (language.length()) {
      DBG("Warning: P-Language header is deprecated."
	  " Please use P-App-Param header instead.\n");
    }
  }

  len = extra_headers.length();
  for (i = 0; i < len; i++) {
    if (extra_headers[i] == '|') extra_headers[i] = '\n';
  }
  if (extra_headers[len - 1] != '\n') {
      extra_headers += '\n';
  }

  if (dialout_suffix.length() == 0) {
    if (!ConferenceFactory::DialoutSuffix.empty()) {
      dialout_suffix = ConferenceFactory::DialoutSuffix;
    } else {
      dialout_suffix = "";
    }
  }
    
  allow_dialout = dialout_suffix.length() > 0;

  listen_only = listen_only_str.length() > 0;

  if (!language.empty()) {

#ifdef USE_MYSQL
    /* Get domain/language specific lonely user file from MySQL */
    if (get_audio_file(LONELY_USER_MSG, req.domain, language,
		       lonely_user_file) &&
	!lonely_user_file.empty()) {
      ConferenceFactory::LonelyUserFile = lonely_user_file;
    } else {
      if (get_audio_file(LONELY_USER_MSG, "", language,
			 lonely_user_file) &&
	  !lonely_user_file.empty()) {
	ConferenceFactory::LonelyUserFile = lonely_user_file;
      }
    }
#else
    /* Get domain/language specific lonely user file from file system */
    lonely_user_file = ConferenceFactory::AudioPath + "/lonely_user_msg/" +
      req.domain + "/" + "default_" + language + ".wav";
    if(file_exists(lonely_user_file)) {
      ConferenceFactory::LonelyUserFile = lonely_user_file;
    } else {
      lonely_user_file = ConferenceFactory::AudioPath +
	"/lonely_user_msg/default_" + language + ".wav";
      if(file_exists(lonely_user_file)) {
	ConferenceFactory::LonelyUserFile = lonely_user_file;
      }
    }
#endif
  }

  DBG("Using LonelyUserFile <%s>\n",
      ConferenceFactory::LonelyUserFile.c_str());

  AmSession::onInvite(req);  
}

void ConferenceDialog::onSessionStart()
{
  //rtp_status = RTP_ingroup;
  
  setupAudio();
  DBG("on start conference dialog\n");
  if(dialedout) {
    // send connect event
    AmSessionContainer::instance()
      ->postEvent(dialout_channel->getConfID(),
		  new DialoutConfEvent(DoConfConnect,
				       dialout_channel->getConfID()));
  }
  //handle connect conf
  if(AmConferenceStatus::checkActiveConference(company_id)) {
    handleRecieveCompanyActive(company_id);
  } else {
    for(set<string>::iterator it = sub_conf_ids.begin(); it != sub_conf_ids.end(); it++) {	
	  if(AmConferenceStatus::checkActiveConference(*it)) {
	    handleRecieveGroupActive(*it);
		
		break;
      }
	}
  }
  
  AmSession::onSessionStart();
}

void ConferenceDialog::setupAudio()
{
  if(!ConferenceFactory::JoinSound.empty()) {
	
    JoinSound.reset(new AmAudioFile());
    if(JoinSound->open(ConferenceFactory::JoinSound,
		       AmAudioFile::Read))
      JoinSound.reset(0);
  }

  if(!ConferenceFactory::DropSound.empty()) {
	
    DropSound.reset(new AmAudioFile());
    if(DropSound->open(ConferenceFactory::DropSound,
		       AmAudioFile::Read))
      DropSound.reset(0);
  }


  play_list.flush();

#if 0
  if(dialout_channel.get()){

    DBG("adding dialout_channel to the playlist (dialedout = %i)\n",dialedout);
    if (listen_only)
	play_list.addToPlaylist(new AmPlaylistItem(dialout_channel.get(),
						   (AmAudio*)NULL));
    else
	play_list.addToPlaylist(new AmPlaylistItem(dialout_channel.get(),
						   dialout_channel.get()));
  }
  else {

    channel.reset(AmConferenceStatus::getChannel(conf_id,getLocalTag(),RTPStream()->getSampleRate()));

    if (listen_only) {
	play_list.addToPlaylist(new AmPlaylistItem(channel.get(),
						   (AmAudio*)NULL));
    }
    else
	play_list.addToPlaylist(new AmPlaylistItem(channel.get(),
						   channel.get()));
#endif

DBG("setup audio\n");
#if 1
	channel.reset(AmConferenceStatus::getChannel(company_id,getLocalTag(),RTPStream()->getSampleRate()));
	play_list.addCompanyToPlaylist(new AmPlaylistItem(channel.get(),
							   channel.get()));

	for (std::set<string>::iterator it=sub_conf_ids.begin(); it!=sub_conf_ids.end(); it++){
                DBG("add channel: %s\n", (*it).c_str());
		AmConferenceChannel* subChannel = AmConferenceStatus::getChannel(*it,getLocalTag(),RTPStream()->getSampleRate());
 		play_list.addToSubPlaylist(*it, new AmPlaylistItem(subChannel,
                                                           subChannel));
		sub_channels.insert(std::pair<string, AmConferenceChannel*>(*it,subChannel));
	}	
#endif
 // }

  DBG("setup audio conf_id: %s", conf_id.c_str());
  //channel.reset(AmConferenceStatus::getChannel("*302",getLocalTag(),RTPStream()->getSampleRate()));
  //play_list.addToPlayListFront(new AmPlaylistItem(channel.get(), channel.get()));

  setInOut(&play_list,&play_list);
    
  setCallgroup(conf_id);
  
  MONITORING_LOG(getLocalTag().c_str(), "conf_id", conf_id.c_str());
	
  if(dialedout || !allow_dialout) {
    DBG("Dialout not enabled or dialout channel. Disabling DTMF detection.\n");
    setDtmfDetectionEnabled(false);
  }
}

void ConferenceDialog::onBye(const AmSipRequest& req)
{
  if(dialout_channel.get())
    disconnectDialout();

  closeChannels();
  setStopped();
}

void ConferenceDialog::process(AmEvent* ev)
{
  DBG("proccess event\n");
  ConferenceEvent* ce = dynamic_cast<ConferenceEvent*>(ev);
  if(ce && ((company_id == ce->conf_id) || (conf_id == ce->conf_id) || (sub_conf_ids.find(ce->conf_id) != sub_conf_ids.end()))){
 DBG("event conf event id: %s\n", ce->conf_id.c_str());
    switch(ce->event_id){
    case ConfNewParticipant:

      DBG("########## new participant #########\n");
#if 0
      if((ce->participants == 1) && 
	 !ConferenceFactory::LonelyUserFile.empty() ){
	if(!LonelyUserFile.get()){
			
	  LonelyUserFile.reset(new AmAudioFile());
	  if(LonelyUserFile->open(ConferenceFactory::LonelyUserFile,
				  AmAudioFile::Read))
	    LonelyUserFile.reset(0);
	}
		
	if(LonelyUserFile.get())
	  play_list.addToPlayListFront(
				       new AmPlaylistItem( LonelyUserFile.get(), NULL ));
      }
      else {
		
	if(JoinSound.get()){
	  JoinSound->rewind();
	  play_list.addToPlayListFront(
				       new AmPlaylistItem( JoinSound.get(), NULL ));
	}
      }
#endif		
      break;
    case ConfParticipantLeft:
      DBG("########## participant left the room #########\n");
	  #if 0
      if(DropSound.get()){
	DropSound->rewind();
	play_list.addToPlayListFront(
				     new AmPlaylistItem( DropSound.get(), NULL ));
      }
	  #endif
      break;
	case GroupActive:
	  handleRecieveGroupActive(ce->conf_id);
    break;
	case CompanyActive:
	  handleRecieveCompanyActive(ce->conf_id);
	break;
	case GroupDeactive:
	  handleRecieveGroupDeactive(ce->conf_id);	
    break;
	case CompanyDeactive:
	  handleRecieveCompanyDeactive(ce->conf_id);
	break;

    default:
      break;
    }
    return;
  }

  DialoutConfEvent* do_ev = dynamic_cast<DialoutConfEvent*>(ev);
  if(do_ev){

    if(dialedout){

      switch(do_ev->event_id){

      case DoConfConnect:
	connectMainChannel();
	break;
	
      case DoConfDisconnect:
		
	dlg->bye();
	closeChannels();
	setStopped();
	break;

      default:
	break;
      }
    }
    else {
	    
      switch(do_ev->event_id){

      case DoConfDisconnect:

	DBG("****** Caller received DoConfDisconnect *******\n");
	connectMainChannel();
	state = CS_normal;
	break;

      case DoConfConnect:

	state = CS_dialout_connected;

	play_list.flush();
	play_list.addToPlaylist(new AmPlaylistItem(dialout_channel.get(),
						   dialout_channel.get()));
	break;

      case DoConfRinging:
		
	if(!RingTone.get())
	  RingTone.reset(new AmRingTone(0,2000,4000,440,480)); // US

	DBG("adding ring tone to the playlist (dialedout = %i)\n",dialedout);
	play_list.flush();
	play_list.addToPlaylist(new AmPlaylistItem(RingTone.get(),NULL));
	break;

      case DoConfError:
		
	DBG("****** Caller received DoConfError *******\n");
	if(!ErrorTone.get())
	  ErrorTone.reset(new AmRingTone(2000,250,250,440,480));

	DBG("adding error tone to the playlist (dialedout = %i)\n",dialedout);
	play_list.addToPlayListFront(new AmPlaylistItem(ErrorTone.get(),NULL));
	break;
		
      }
    }

    return;
  }

  AmSession::process(ev);
}

string dtmf2str(int event)
{
  switch(event){
  case 0: case 1: case 2:
  case 3: case 4: case 5:
  case 6: case 7: case 8:
  case 9:
    return int2str(event);
	
  case 10: return "*";
  case 11: return "#";
  default: return "";
  }
}


void ConferenceDialog::onDtmf(int event, int duration)
{
  DBG("ConferenceDialog::onDtmf\n");
  if (dialedout || !allow_dialout ||
      ((ConferenceFactory::MaxParticipants > 0) &&
       (AmConferenceStatus::getConferenceSize(dlg->getUser()) >= 
	ConferenceFactory::MaxParticipants)))
    return;

  switch(state){
	
  case CS_normal:
    DBG("CS_normal\n");
    //DBG("start test code 1\n");

    //ConferenceDialog* s = new ConferenceDialog("*302");

    //setupSessionTimer(s);   
   // DBG("end test \n");
    //dtmf_seq += dtmf2str(event);
	if(event == DTMF_company) {
		DBG("DTMF_company\n");
		connectToCompany();
	}

	if(event == DTMF_cancel_company){
		DBG("DTMF_cancel_company\n");
		cancelConnectCompany();
	}

	if(event == DTMF_group) {
		DBG("DTMF DTMF_group\n");
		connectToGroup();
	}

    if(event == DTMF_cancel_group) {
		DBG("DTMF DTMF_cancel_group\n");
		cancelConnectGroup();
	}
#if 0
    if(dtmf_seq.length() == 2){

#ifdef WITH_SAS_TTS
      if((dtmf_seq == "##") && !last_sas.empty()) {
	sayTTS(last_sas);
	dtmf_seq = "";
      }
#endif

      if(dtmf_seq == "#*") {
	state = CS_dialing_out;
	dtmf_seq = "";
      } else {
	// keep last digit
	dtmf_seq = dtmf_seq[1]; 
      }
    }
    break;

  case CS_dialing_out:{
    DBG("CS_dialing_out\n");
    string digit = dtmf2str(event);

    if(digit == "*"){
	    
      if(!dtmf_seq.empty()){
	createDialoutParticipant(dtmf_seq);
	state = CS_dialed_out;
      }
      else {
	DBG("state = CS_normal; ????????\n");
	state = CS_normal;
      }

      dtmf_seq = "";
    }
    else 
      dtmf_seq += digit;

  } break;


  case CS_dialout_connected:
    DBG("CS_dialout_connected\n");
    if(event == 10){ // '*'

      AmSessionContainer::instance()
	->postEvent(dialout_id,
		    new DialoutConfEvent(DoConfConnect,
					 getLocalTag()));

      connectMainChannel();
      state = CS_normal;
    }

  case CS_dialed_out:
    DBG("CS_dialed_out\n");
    if(event == 11){ // '#'
      disconnectDialout();
      state = CS_normal;
    }
#endif
    break;
	
  }
}

void ConferenceDialog::connectChannelByUri(const string& uri){
  DBG("connect uri: %s\n", uri.c_str());
  play_list.flush();
  channel.reset(AmConferenceStatus::getChannel(uri,getLocalTag(),RTPStream()->getSampleRate()));
  play_list.addToPlayListFront(new AmPlaylistItem(channel.get(), channel.get()));
}

void ConferenceDialog::connectToGroup(){
  DBG("connectToGroup ptt: %d - rtp_recv: %d\n", ptt_status, rtp_recv);
  if(rtp_recv == RTP_group || rtp_recv == RTP_company || ptt_status == PTT_group || ptt_status == PTT_company)
  	return;
  bool has_active = false;
  for(set<string>::iterator it = sub_conf_ids.begin(); it != sub_conf_ids.end(); it++) {
	if(!AmConferenceStatus::setActiveConferenceReturnStatus(*it, true)) {
      has_active = true;
	  break;
	}
  }
  
  for(set<string>::iterator it = sub_conf_ids.begin(); it != sub_conf_ids.end(); it++) {
  	if(has_active) {
	  AmConferenceStatus::setActiveConferenceReturnStatus(*it, false);
	} else {
	  AmConferenceStatus::postConferenceEvent(*it, GroupActive, getLocalTag());
	}	
  }

  if(has_active) {
  	DBG("has one active\n");
    //has one active - send busy
	return;
  }

  DBG("start connectToGroup\n");
  play_list.PutToGroupChannel(true);
  
  ptt_status = PTT_group;
  DBG("end connectToGroup\n");
}

void ConferenceDialog::handleRecieveGroupActive(string cid)
{
	DBG("########## hello hello GroupActive room: %s ce %s #########\n", conf_id.c_str(), cid.c_str());
	if(active_room != "") {
		DBG("send busy\n");
	  //send busy
	  return;
	}
	active_room = cid;
	if(ptt_status != PTT_group) {
	  rtp_recv = RTP_group;
	  play_list.setActiveGetChannel(cid);
	} else {
	  DBG("is not PTT_group\n");
	}
}

void ConferenceDialog::handleRecieveGroupDeactive(string cid)
{
	DBG("########## GroupDeactive room: %s ce %s #########\n", conf_id.c_str(), cid.c_str());
					
	if(active_room == cid)
	   active_room = "";

	if(ptt_status != PTT_cancel_group) {
	  rtp_recv = RTP_cancel_group;
	  play_list.setDeactiveGetChannel(cid);
	} else {
	  DBG("is not PTT_cancel_group\n");
	}

}

void ConferenceDialog::handleRecieveCompanyActive(string cid)
{
	DBG("########## CompanyActive room: %s ce %s #########\n", company_id.c_str(), cid.c_str());
	if(active_room == company_id) {
	  //send busy
	  DBG("send busy\n");
	  return;
	}
	
	cancelConnectGroup();
	active_room = cid;
	if(ptt_status != PTT_company) {
	  rtp_recv = RTP_company;
	  play_list.setActiveGetCompanyChannel(true);
	} else {
	  DBG("is not PTT_company\n");
	}
}

void ConferenceDialog::handleRecieveCompanyDeactive(string cid)
{
	DBG("########## CompanyDeactive room: %s ce %s #########\n", company_id.c_str(), cid.c_str());
	if(active_room == cid)
	   active_room = "";

	if(ptt_status != PTT_cancel_company) {
	  rtp_recv = RTP_cancel_company;
	  play_list.setActiveGetCompanyChannel(false);
	} else {
	  DBG("is not PTT_cancel_company\n");
	}
}

void ConferenceDialog::cancelConnectGroup()
{
  DBG("cancelConnectGroup ptt: %d - rtp_recv: %d", ptt_status, rtp_recv);

  if(ptt_status != PTT_group)
  	return;

  DBG("start cancelConnectGroup\n");
  play_list.PutToGroupChannel(false);
  for(set<string>::iterator it = sub_conf_ids.begin(); it != sub_conf_ids.end(); it++) {
	AmConferenceStatus::postConferenceEvent(*it, GroupDeactive, getLocalTag());
	AmConferenceStatus::setActiveConferenceReturnStatus(*it, false);
  }
  
  ptt_status = PTT_cancel_group;
  DBG("end cancelConnectGroup\n");
}

void ConferenceDialog::connectToCompany(){
  DBG("connect company ptt: %d - rtp_recv: %d", ptt_status, rtp_recv);
  if(ptt_status == PTT_company || rtp_recv == RTP_company)
	return;

  if(!AmConferenceStatus::setActiveConferenceReturnStatus(company_id, true)) {
    DBG("Has one active\n");    
    //has one active - send busy
    return;
  }
 
  cancelConnectGroup(); 
  DBG("start connect\n");
  play_list.PutToCompanyChannel(true);
  AmConferenceStatus::postConferenceEvent(company_id, CompanyActive, getLocalTag());
  
  ptt_status = PTT_company;
  DBG("end connect\n");
}

void ConferenceDialog::cancelConnectCompany(){
  DBG("cancelConnectCompany ptt: %d - rtp_recv: %d", ptt_status, rtp_recv);
  if(ptt_status != PTT_company)
		return;

  DBG("start cancelConnectCompany\n");
  play_list.PutToCompanyChannel(false);
  AmConferenceStatus::postConferenceEvent(company_id, CompanyDeactive, getLocalTag());
  AmConferenceStatus::setActiveConferenceReturnStatus(company_id, false);
  
  ptt_status = PTT_cancel_company;
  DBG("end cancelConnectCompany\n");
}

void ConferenceDialog::setCompanyId(string id){

  company_id = id;
}

void ConferenceDialog::addSubConf(string id){
AmEventDispatcher::instance()->
     addEventQueue("*301", this);
  //AmSessionContainer::instance()->addSession("*301", this);
  sub_conf_ids.insert(id);
}

set<string> ConferenceDialog::getSubConf(){
  return sub_conf_ids;
}

string ConferenceDialog::getCompanyId(){
  return company_id;
}

void ConferenceDialog::connectToAll(){
  //if(rtp_status != RTP_ingroup)
  //	return;
  
  //connectChannelByUri("*30*");
  
  //rtp_status = RTP_inall;
}

int ConferenceDialog::writeStreams(unsigned long long ts, unsigned char *buffer) 
{
#if 0 
  if(!conf_id_active.empty()){
    DBG("active: %s\n", conf_id_active.front().c_str());
    DBG("active chl: %s\n", play_list.getActiveChannel().c_str());

    if(play_list.getActiveChannel() != conf_id_active.front())
    play_list.setActiveGetChannel(conf_id_active.front());
  }
#endif   
  int res = 0;
  lockAudio();

  AmRtpAudio *stream = RTPStream();
  if (stream->sendIntReached()) { // FIXME: shouldn't depend on checkInterval call before!
    unsigned int f_size = stream->getFrameSize();
    int got = 10;
#if 1
    if (output) got = play_list.get(ts, buffer, stream->getSampleRate(), f_size);
    if (got < 0) res = -1;
    if (got > 0){
      res = stream->put(ts, buffer, stream->getSampleRate(), got);
    }
#endif
  }
  
  unlockAudio();
  return res;
}

void ConferenceDialog::createDialoutParticipant(const string& uri_user)
{
  string uri;

  uri = "sip:" + uri_user + dialout_suffix;

  dialout_channel.reset(AmConferenceStatus::getChannel(getLocalTag(),getLocalTag(),RTPStream()->getSampleRate()));

  dialout_id = AmSession::getNewId();
  
  DBG("Create participant: conference id\n");  
  ConferenceDialog* dialout_session = 
    new ConferenceDialog(conf_id,
			 AmConferenceStatus::getChannel(getLocalTag(),
							dialout_id,RTPStream()->getSampleRate()));

  ConferenceFactory::setupSessionTimer(dialout_session);

  AmSipDialog* dialout_dlg = dialout_session->dlg;

  dialout_dlg->setLocalTag(dialout_id);
  dialout_dlg->setCallid(AmSession::getNewId());

  if (from_header.length() > 0) {
    dialout_dlg->setLocalParty(from_header);
  } else {
    dialout_dlg->setLocalParty(dlg->getLocalParty());
  }
  dialout_dlg->setRemoteParty(uri);
  dialout_dlg->setRemoteUri(uri);

  dialout_dlg->sendRequest(SIP_METH_INVITE,NULL,
			  extra_headers);

  dialout_session->start();

  AmSessionContainer* sess_cont = AmSessionContainer::instance();
  sess_cont->addSession(dialout_id,dialout_session);
}

void ConferenceDialog::disconnectDialout()
{
  if(dialedout){
	
    if(dialout_channel.get()){
	    
      AmSessionContainer::instance()
	->postEvent(dialout_channel->getConfID(),
		    new DialoutConfEvent(DoConfDisconnect,
					 dialout_channel->getConfID()));
    }
  }
  else {

    AmSessionContainer::instance()
      ->postEvent(dialout_id,
		  new DialoutConfEvent(DoConfDisconnect,
				       getLocalTag()));
    
    connectMainChannel();
  }
}

void ConferenceDialog::connectMainChannel()
{
  dialout_id = "";
  dialedout = false;
  dialout_channel.reset(NULL);
    
  play_list.flush();

  if(!channel.get())
    channel.reset(AmConferenceStatus
		  ::getChannel(conf_id,
			       getLocalTag(),RTPStream()->getSampleRate()));

  play_list.addToPlaylist(new AmPlaylistItem(channel.get(),
					     channel.get()));
}

void ConferenceDialog::closeChannels()
{
  play_list.flush();
  setInOut(NULL,NULL);
  channel.reset(NULL);
  dialout_channel.reset(NULL);
  for (map<string, AmConferenceChannel*>::iterator it=sub_channels.begin(); it!=sub_channels.end(); it++) {
		 delete it->second;
  }
}

void ConferenceDialog::onSipRequest(const AmSipRequest& req)
{
  AmSession::onSipRequest(req);
  if((dlg->getStatus() >= AmSipDialog::Connected) ||
     (req.method != "REFER"))
    return;

  string local_party(dlg->getLocalParty());
  dlg->setLocalParty(dlg->getRemoteParty());
  dlg->setRemoteParty(local_party);
  dlg->setRemoteTag("");

  // get route set and next hop
  string iptel_app_param = getHeader(req.hdrs, PARAM_HDR, true);
  if (iptel_app_param.length()) {
    dlg->setRouteSet(get_header_keyvalue(iptel_app_param,"Transfer-RR"));
  } else {
    INFO("Use of P-Transfer-RR/P-Transfer-NH is deprecated. "
	 "Use '%s: Transfer-RR=<rr>;Transfer-NH=<nh>' instead.\n",PARAM_HDR);

    dlg->setRouteSet(getHeader(req.hdrs,"P-Transfer-RR", true));
  }

  DBG("ConferenceDialog::onSipRequest: local_party = %s\n",dlg->getLocalParty().c_str());
  DBG("ConferenceDialog::onSipRequest: local_tag = %s\n",dlg->getLocalTag().c_str());
  DBG("ConferenceDialog::onSipRequest: remote_party = %s\n",dlg->getRemoteParty().c_str());
  DBG("ConferenceDialog::onSipRequest: remote_tag = %s\n",dlg->getRemoteTag().c_str());

  dlg->sendRequest(SIP_METH_INVITE);

  transfer_req.reset(new AmSipRequest(req));

  return;
}

void ConferenceDialog::onSipReply(const AmSipRequest& req,
				  const AmSipReply& reply, 
				  AmBasicSipDialog::Status old_dlg_status)
{
  AmSession::onSipReply(req, reply, old_dlg_status);

  DBG("ConferenceDialog::onSipReply: code = %i, reason = %s\n, status = %i\n",
      reply.code,reply.reason.c_str(),dlg->getStatus());
    
  if(!dialedout /*&& !transfer_req.get()*/)
    return;

  if((old_dlg_status < AmSipDialog::Connected) &&
     (reply.cseq_method == SIP_METH_INVITE)){

    switch(dlg->getStatus()){

    case AmSipDialog::Proceeding:
    case AmSipDialog::Early:

      switch(reply.code){
      case 180:
      case 183: break;//TODO: remote ring tone.

	if(dialout_channel.get()){
	  // send ringing event
	  AmSessionContainer::instance()
	    ->postEvent(dialout_channel->getConfID(),
			new DialoutConfEvent(DoConfRinging,
					     dialout_channel->getConfID()));
	}
		
	break;
      default:  break;// continue waiting.
      }
      break;

    case AmSipDialog::Disconnected:

      // if(!transfer_req.get()){
      
      if(dialout_channel.get()){
	disconnectDialout();
	AmSessionContainer::instance()
	  ->postEvent(dialout_channel->getConfID(),
		      new DialoutConfEvent(DoConfError,
					   dialout_channel->getConfID()));
      }
      setStopped();

      // }
      // else {
      // 	dlg->reply(*(transfer_req.get()),reply.code,reply.reason);
      // 	transfer_req.reset(0);
      // 	setStopped();
      // }
      break;

    default: break;
    }
  }
}

#ifdef WITH_SAS_TTS
void ConferenceDialog::onZRTPEvent(zrtp_event_t event, zrtp_stream_ctx_t *stream_ctx) {
  DBG("ZrtpConferenceDialog::onZRTPEvent \n");

  switch (event) {
  case ZRTP_EVENT_IS_SECURE: {
    INFO("ZRTP_EVENT_IS_SECURE \n");
    //         info->is_verified  = ctx->_session_ctx->secrets.verifieds & ZRTP_BIT_RS0;
    
    zrtp_conn_ctx_t *session = stream_ctx->_session_ctx;
    
    string tts_sas = "My SAS is ";
    
    if (ZRTP_SAS_BASE32 == session->sas_values.rendering) {
      DBG("Got SAS value <<<%.4s>>>\n", session->sas_values.str1.buffer);
      tts_sas += session->sas_values.str1.buffer;
    } else {
      DBG("Got SAS values SAS1 '%s' and SAS2 '%s'\n", 
	  session->sas_values.str1.buffer,
	  session->sas_values.str2.buffer);
      tts_sas += session->sas_values.str1.buffer + string(" and ") + 
	session->sas_values.str2.buffer + ".";
    }
    
    sayTTS(tts_sas);
    return;
  } break;
  default: break;
  } 
  AmSession::onZRTPEvent(event, stream_ctx);  
}

void ConferenceDialog::sayTTS(string text) {

  string filename = string(TTS_CACHE_PATH) + text /* AmSession::getNewId() */
    + string(".wav");

  last_sas = text;
  flite_text_to_speech(text.c_str(),tts_voice,filename.c_str());
  
  AmAudioFile* af = new AmAudioFile();
  if(!af->open(filename.c_str(), AmAudioFile::Read)) {
    play_list.addToPlayListFront(new AmPlaylistItem(af, NULL));
    TTSFiles.push_back(af);
  } else {
    ERROR("ERROR reading TTSed file %s\n", filename.c_str());
    delete af;
  }
}

#endif // WITH_SAS_TTS
