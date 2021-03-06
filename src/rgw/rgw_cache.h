#ifndef CEPH_RGWCACHE_H
#define CEPH_RGWCACHE_H

#include "rgw_access.h"
#include <string>
#include <map>
#include "include/types.h"
#include "include/utime.h"

enum {
  UPDATE_OBJ,
  REMOVE_OBJ,
};

#define CACHE_FLAG_DATA           0x1
#define CACHE_FLAG_XATTRS         0x2
#define CACHE_FLAG_META           0x4
#define CACHE_FLAG_APPEND_XATTRS  0x8

struct ObjectMetaInfo {
  uint64_t size;
  time_t mtime;

  ObjectMetaInfo() : size(0), mtime(0) {}

  void encode(bufferlist& bl) const {
    __u8 struct_v = 1;
    ::encode(struct_v, bl);
    ::encode(size, bl);
    utime_t t(mtime, 0);
    ::encode(t, bl);
  }
  void decode(bufferlist::iterator& bl) {
    __u8 struct_v;
    ::decode(struct_v, bl);
    ::decode(size, bl);
    utime_t t;
    ::decode(t, bl);
    mtime = t.sec();
  }
};
WRITE_CLASS_ENCODER(ObjectMetaInfo)

struct ObjectCacheInfo {
  int status;
  uint32_t flags;
  bufferlist data;
  map<string, bufferlist> xattrs;
  ObjectMetaInfo meta;

  ObjectCacheInfo() : status(0), flags(0) {}

  void encode(bufferlist& bl) const {
    __u8 struct_v = 1;
    ::encode(struct_v, bl);
    ::encode(status, bl);
    ::encode(flags, bl);
    ::encode(data, bl);
    ::encode(xattrs, bl);
    ::encode(meta, bl);
  }
  void decode(bufferlist::iterator& bl) {
    __u8 struct_v;
    ::decode(struct_v, bl);
    ::decode(status, bl);
    ::decode(flags, bl);
    ::decode(data, bl);
    ::decode(xattrs, bl);
    ::decode(meta, bl);
  }
};
WRITE_CLASS_ENCODER(ObjectCacheInfo)

struct RGWCacheNotifyInfo {
  uint32_t op;
  rgw_obj obj;
  ObjectCacheInfo obj_info;
  off_t ofs;
  string ns;

  RGWCacheNotifyInfo() : op(0), ofs(0) {}

  void encode(bufferlist& obl) const {
    __u8 struct_v = 1;
    ::encode(struct_v, obl);
    ::encode(op, obl);
    ::encode(obj, obl);
    ::encode(obj_info, obl);
    ::encode(ofs, obl);
    ::encode(ns, obl);
  }
  void decode(bufferlist::iterator& ibl) {
    __u8 struct_v;
    ::decode(struct_v, ibl);
    ::decode(op, ibl);
    ::decode(obj, ibl);
    ::decode(obj_info, ibl);
    ::decode(ofs, ibl);
    ::decode(ns, ibl);
  }
};
WRITE_CLASS_ENCODER(RGWCacheNotifyInfo)

struct ObjectCacheEntry {
  ObjectCacheInfo info;
  std::list<string>::iterator lru_iter;
};

class ObjectCache {
  std::map<string, ObjectCacheEntry> cache_map;
  std::list<string> lru;
  Mutex lock;

  void touch_lru(string& name, std::list<string>::iterator& lru_iter);
  void remove_lru(string& name, std::list<string>::iterator& lru_iter);
public:
  ObjectCache() : lock("ObjectCache") { }
  int get(std::string& name, ObjectCacheInfo& bl, uint32_t mask);
  void put(std::string& name, ObjectCacheInfo& bl);
  void remove(std::string& name);
};

static inline void normalize_bucket_and_obj(rgw_bucket& src_bucket, string& src_obj, rgw_bucket& dst_bucket, string& dst_obj)
{
  if (src_obj.size()) {
    dst_bucket = src_bucket;
    dst_obj = src_obj;
  } else {
    dst_bucket = rgw_root_bucket;
    dst_obj = src_bucket.name;
  }
}

template <class T>
class RGWCache  : public T
{
  ObjectCache cache;

  int list_objects_raw_init(rgw_bucket& bucket, RGWAccessHandle *handle) {
    return T::list_objects_raw_init(bucket, handle);
  }
  int list_objects_raw_next(RGWObjEnt& obj, RGWAccessHandle *handle) {
    return T::list_objects_raw_next(obj, handle);
  }

  string normal_name(rgw_bucket& bucket, std::string& oid) {
    string& bucket_name = bucket.name;
    char buf[bucket_name.size() + 1 + oid.size() + 1];
    const char *bucket_str = bucket_name.c_str();
    const char *oid_str = oid.c_str();
    sprintf(buf, "%s+%s", bucket_str, oid_str);
    return string(buf);
  }

  string normal_name(rgw_obj& obj) {
    return normal_name(obj.bucket, obj.object);
  }

  int initialize(CephContext *cct) {
    int ret;
    ret = T::initialize(cct);
    if (ret < 0)
      return ret;

    ret = T::init_watch();
    return ret;
  }

  void finalize() {
    T::finalize_watch();
  }
  int distribute(rgw_obj& obj, ObjectCacheInfo& obj_info, int op);
  int watch_cb(int opcode, uint64_t ver, bufferlist& bl);
public:
  RGWCache() {}

  int set_attr(void *ctx, rgw_obj& obj, const char *name, bufferlist& bl);
  int put_obj_meta(void *ctx, rgw_obj& obj, uint64_t size, time_t *mtime,
                   map<std::string, bufferlist>& attrs, RGWObjCategory category, bool exclusive,
                   map<std::string, bufferlist>* rmattrs, const bufferlist *data);

  int put_obj_data(void *ctx, rgw_obj& obj, const char *data,
              off_t ofs, size_t len, bool exclusive);

  int get_obj(void *ctx, void **handle, rgw_obj& obj, char **data, off_t ofs, off_t end);

  int obj_stat(void *ctx, rgw_obj& obj, uint64_t *psize, time_t *pmtime, map<string, bufferlist> *attrs, bufferlist *first_chunk);

  int delete_obj(void *ctx, rgw_obj& obj, bool sync);
};


template <class T>
int RGWCache<T>::delete_obj(void *ctx, rgw_obj& obj, bool sync)
{
  rgw_bucket bucket;
  string oid;
  normalize_bucket_and_obj(obj.bucket, obj.object, bucket, oid);
  if (bucket.name[0] != '.')
    return T::delete_obj(ctx, obj, sync);

  string name = normal_name(obj);
  cache.remove(name);

  ObjectCacheInfo info;
  distribute(obj, info, REMOVE_OBJ);

  return T::delete_obj(ctx, obj, sync);
}

template <class T>
int RGWCache<T>::get_obj(void *ctx, void **handle, rgw_obj& obj, char **data, off_t ofs, off_t end)
{
  rgw_bucket bucket;
  string oid;
  normalize_bucket_and_obj(obj.bucket, obj.object, bucket, oid);
  if (bucket.name[0] != '.' || ofs != 0)
    return T::get_obj(ctx, handle, obj, data, ofs, end);

  string name = normal_name(obj.bucket, oid);

  ObjectCacheInfo info;
  if (cache.get(name, info, CACHE_FLAG_DATA) == 0) {
    if (info.status < 0)
      return info.status;

    bufferlist& bl = info.data;

    *data = (char *)malloc(bl.length());
    memcpy(*data, bl.c_str(), bl.length());
    return bl.length();
  }
  int r = T::get_obj(ctx, handle, obj, data, ofs, end);
  if (r < 0) {
    if (r == -ENOENT) { // only update ENOENT, we'd rather retry other errors
      info.status = r;
      cache.put(name, info);
    }
    return r;
  }

  bufferptr p(r);
  bufferlist& bl = info.data;
  memcpy(p.c_str(), *data, r);
  bl.clear();
  bl.append(p);
  info.status = 0;
  info.flags = CACHE_FLAG_DATA;
  cache.put(name, info);
  return r;
}

template <class T>
int RGWCache<T>::set_attr(void *ctx, rgw_obj& obj, const char *attr_name, bufferlist& bl)
{
  rgw_bucket bucket;
  string oid;
  normalize_bucket_and_obj(obj.bucket, obj.object, bucket, oid);
  ObjectCacheInfo info;
  bool cacheable = false;
  if (bucket.name[0] == '.') {
    cacheable = true;
    info.xattrs[attr_name] = bl;
    info.status = 0;
    info.flags = CACHE_FLAG_APPEND_XATTRS;
  }
  int ret = T::set_attr(ctx, obj, attr_name, bl);
  if (cacheable) {
    string name = normal_name(bucket, oid);
    if (ret >= 0) {
      cache.put(name, info);
      int r = distribute(obj, info, UPDATE_OBJ);
      if (r < 0)
        dout(0) << "ERROR: failed to distribute cache for " << obj << dendl;
    } else {
     cache.remove(name);
    }
  }

  return ret;
}

template <class T>
int RGWCache<T>::put_obj_meta(void *ctx, rgw_obj& obj, uint64_t size, time_t *mtime,
                              map<std::string, bufferlist>& attrs, RGWObjCategory category, bool exclusive,
                              map<std::string, bufferlist>* rmattrs, const bufferlist *data)
{
  rgw_bucket bucket;
  string oid;
  normalize_bucket_and_obj(obj.bucket, obj.object, bucket, oid);
  ObjectCacheInfo info;
  bool cacheable = false;
  if (bucket.name[0] == '.') {
    cacheable = true;
    info.xattrs = attrs;
    info.status = 0;
    info.flags = CACHE_FLAG_XATTRS;
    if (data) {
      info.data = *data;
      info.flags |= CACHE_FLAG_DATA;
    }
  }
  int ret = T::put_obj_meta(ctx, obj, size, mtime, attrs, category, exclusive, rmattrs, data);
  if (cacheable) {
    string name = normal_name(bucket, oid);
    if (ret >= 0) {
      cache.put(name, info);
      int r = distribute(obj, info, UPDATE_OBJ);
      if (r < 0)
        dout(0) << "ERROR: failed to distribute cache for " << obj << dendl;
    } else {
     cache.remove(name);
    }
  }

  return ret;
}

template <class T>
int RGWCache<T>::put_obj_data(void *ctx, rgw_obj& obj, const char *data,
              off_t ofs, size_t len, bool exclusive)
{
  rgw_bucket bucket;
  string oid;
  normalize_bucket_and_obj(obj.bucket, obj.object, bucket, oid);
  ObjectCacheInfo info;
  bool cacheable = false;
  if ((bucket.name[0] == '.') && ((ofs == 0) || (ofs == -1))) {
    cacheable = true;
    bufferptr p(len);
    memcpy(p.c_str(), data, len);
    bufferlist& bl = info.data;
    bl.append(p);
    info.meta.size = bl.length();
    info.status = 0;
    info.flags = CACHE_FLAG_DATA;
  }
  int ret = T::put_obj_data(ctx, obj, data, ofs, len, exclusive);
  if (cacheable) {
    string name = normal_name(bucket, oid);
    if (ret >= 0) {
      cache.put(name, info);
      int r = distribute(obj, info, UPDATE_OBJ);
      if (r < 0)
        dout(0) << "ERROR: failed to distribute cache for " << obj << dendl;
    } else {
     cache.remove(name);
    }
  }

  return ret;
}

template <class T>
int RGWCache<T>::obj_stat(void *ctx, rgw_obj& obj, uint64_t *psize, time_t *pmtime, map<string, bufferlist> *attrs, bufferlist *first_chunk)
{
  rgw_bucket bucket;
  string oid;
  normalize_bucket_and_obj(obj.bucket, obj.object, bucket, oid);
  if (bucket.name[0] != '.')
    return T::obj_stat(ctx, obj, psize, pmtime, attrs, first_chunk);

  string name = normal_name(bucket, oid);

  uint64_t size;
  time_t mtime;

  ObjectCacheInfo info;
  int r = cache.get(name, info, CACHE_FLAG_META | CACHE_FLAG_XATTRS);
  if (r == 0) {
    if (info.status < 0)
      return info.status;

    size = info.meta.size;
    mtime = info.meta.mtime;
    goto done;
  }
  r = T::obj_stat(ctx, obj, &size, &mtime, &info.xattrs, first_chunk);
  if (r < 0) {
    if (r == -ENOENT) {
      info.status = r;
      cache.put(name, info);
    }
    return r;
  }
  info.status = 0;
  info.meta.mtime = mtime;
  info.meta.size = size;
  info.flags = CACHE_FLAG_META | CACHE_FLAG_XATTRS;
  cache.put(name, info);
done:
  if (psize)
    *psize = size;
  if (pmtime)
    *pmtime = mtime;
  if (attrs)
    *attrs = info.xattrs;
  return 0;
}

template <class T>
int RGWCache<T>::distribute(rgw_obj& obj, ObjectCacheInfo& obj_info, int op)
{
  RGWCacheNotifyInfo info;

  info.op = op;

  info.obj_info = obj_info;
  info.obj = obj;
  bufferlist bl;
  ::encode(info, bl);
  int ret = T::distribute(bl);
  return ret;
}

template <class T>
int RGWCache<T>::watch_cb(int opcode, uint64_t ver, bufferlist& bl)
{
  RGWCacheNotifyInfo info;

  try {
    bufferlist::iterator iter = bl.begin();
    ::decode(info, iter);
  } catch (buffer::end_of_buffer& err) {
    dout(0) << "ERROR: got bad notification" << dendl;
    return -EIO;
  } catch (buffer::error& err) {
    dout(0) << "ERROR: buffer::error" << dendl;
    return -EIO;
  }

  string name = normal_name(info.obj);

  switch (info.op) {
  case UPDATE_OBJ:
    cache.put(name, info.obj_info);
    break;
  case REMOVE_OBJ:
    cache.remove(name);
    break;
  default:
    dout(0) << "WARNING: got unknown notification op: " << info.op << dendl;
    return -EINVAL;
  }

  return 0;
}

#endif
