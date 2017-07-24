#ifndef ConferencePlaylist_h
#define ConferencePlaylist_h

#include "AmPlaylist.h"

#include <map>
#include <string>
using std::map;
using std::string;

class ConferencePlaylist : public AmPlaylist
{
  AmMutex                items_mut;
  deque<AmPlaylistItem*> items;

  AmMutex                cur_mut;
  AmPlaylistItem*        cur_item;

  AmEventQueue*          ev_q;

  AmMutex                company_mut;
  AmPlaylistItem*        company_item;

  AmMutex                sub_items_mut;
  map<string, AmPlaylistItem*>   sub_items;
  string                 activeChannel;
  AmMutex                channel_mut;

  bool                   put_group_channel;
  AmMutex                put_channel_mut;

  AmMutex                get_company_channel_mut;
  bool                   put_company_channel;
  bool                   get_company_channel;
  
protected:
  /** Fake implement AmAudio's pure virtual methods */
  int read(unsigned int user_ts, unsigned int size){ return -1; }
  int write(unsigned int user_ts, unsigned int size){ return -1; }

  /** override AmAudio */
  int get(unsigned long long system_ts, unsigned char* buffer, 
  int output_sample_rate, unsigned int nb_samples);

  int put(unsigned long long system_ts, unsigned char* buffer, 
  int input_sample_rate, unsigned int size);

  /** from AmAudio */
  void close();

public:
  void addCompanyToPlaylist(AmPlaylistItem* item);
  void addToSubPlaylist(string conf, AmPlaylistItem* item);
  void setActiveGetChannel(string channel);
  void setDeactiveGetChannel(string channel);
  void PutToGroupChannel(bool is_put);
  void PutToCompanyChannel(bool is_put);
  void setActiveGetCompanyChannel(bool is_get);
  void flushChannel();
};

