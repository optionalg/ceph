// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2004-2006 Sage Weil <sage@newdream.net>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software 
 * Foundation.  See file COPYING.
 * 
 */

#ifndef CEPH_MON_SESSION_H
#define CEPH_MON_SESSION_H

#include "include/xlist.h"
#include "msg/msg_types.h"

#include "auth/AuthServiceHandler.h"

#include "MonCaps.h"

struct MonSession;

struct Subscription {
  MonSession *session;
  string type;
  xlist<Subscription*>::item type_item;
  version_t next;
  bool onetime;
  bool incremental_onetime;  // has CEPH_FEATURE_INCSUBOSDMAP
  
  Subscription(MonSession *s, const string& t) : session(s), type(t), type_item(this),
						 next(0), onetime(false), incremental_onetime(false) {};
};

struct MonSession : public RefCountedObject {
  Connection *con;
  entity_inst_t inst;
  utime_t until;
  utime_t time_established;
  bool closed;
  xlist<MonSession*>::item item;
  set<uint64_t> routed_request_tids;
  MonCaps caps;
  uint64_t global_id;
  uint64_t notified_global_id;

  map<string, Subscription*> sub_map;

  AuthServiceHandler *auth_handler;

  Connection *proxy_con;
  uint64_t proxy_tid;

  MonSession(entity_inst_t i, Connection *c) :
    con(c->get()), inst(i), closed(false), item(this),
    global_id(0), notified_global_id(0), auth_handler(NULL),
    proxy_con(NULL), proxy_tid(0) {
    time_established = ceph_clock_now(g_ceph_context);
  }
  ~MonSession() {
    if (con)
      con->put();
    if (proxy_con)
      proxy_con->put();
    //generic_dout(0) << "~MonSession " << this << dendl;
    // we should have been removed before we get destructed; see MonSessionMap::remove_session()
    assert(!item.is_on_list());
    assert(sub_map.empty());
    delete auth_handler;
  }
};


struct MonSessionMap {
  xlist<MonSession*> sessions;
  map<string, xlist<Subscription*>* > subs;
  multimap<int, MonSession*> by_osd;

  void remove_session(MonSession *s) {
    assert(!s->closed);
    for (map<string,Subscription*>::iterator p = s->sub_map.begin(); p != s->sub_map.end(); ++p) {
      p->second->type_item.remove_myself();
      delete p->second;
    }
    s->sub_map.clear();
    s->item.remove_myself();
    if (s->inst.name.is_osd()) {
      for (multimap<int,MonSession*>::iterator p = by_osd.find(s->inst.name.num());
	   p->first == s->inst.name.num();
	   p++)
	if (p->second == s) {
	  by_osd.erase(p);
	  break;
	}
    }
    s->closed = true;
    s->put();
  }

  MonSession *new_session(entity_inst_t i, Connection *c) {
    MonSession *s = new MonSession(i, c);
    sessions.push_back(&s->item);
    if (i.name.is_osd())
      by_osd.insert(pair<int,MonSession*>(i.name.num(), s));
    s->get();  // caller gets a ref
    return s;
  }
  
  MonSession *get_random_osd_session() {
    // ok, this isn't actually random, but close enough.
    if (by_osd.empty())
      return 0;
    int n = by_osd.rbegin()->first + 1;
    int r = rand() % n;
    multimap<int,MonSession*>::iterator p = by_osd.lower_bound(r);
    if (p == by_osd.end())
      p--;
    return p->second;
  }


  void add_update_sub(MonSession *s, const string& what, version_t start, bool onetime, bool incremental_onetime) {
    Subscription *sub = 0;
    if (s->sub_map.count(what)) {
      sub = s->sub_map[what];
    } else {
      sub = new Subscription(s, what);
      s->sub_map[what] = sub;
      
      if (!subs.count(what))
	subs[what] = new xlist<Subscription*>;
      subs[what]->push_back(&sub->type_item);
    }
    sub->next = start;
    sub->onetime = onetime;
    sub->incremental_onetime = onetime && incremental_onetime;
  }

  void remove_sub(Subscription *sub) {
    sub->session->sub_map.erase(sub->type);
    sub->type_item.remove_myself();
    delete sub;
  }
};

inline ostream& operator<<(ostream& out, const MonSession *s)
{
  out << "MonSession: " << s->inst << " is "
      << (s->closed ? "closed" : "open");
  out << s->caps;
  return out;
}

#endif
