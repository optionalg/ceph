#include "common/Clock.h"
#include "common/utf8.h"

#include "rgw_log.h"
#include "rgw_acl.h"
#include "rgw_access.h"

#define DOUT_SUBSYS rgw

static rgw_bucket log_bucket(RGW_LOG_POOL_NAME);

static void set_param_str(struct req_state *s, const char *name, string& str)
{
  const char *p = s->env->get(name);
  if (p)
    str = p;
}

string render_log_object_name(const string& format,
			      struct tm *dt, int64_t bucket_id, const string& bucket_name)
{
  string o;
  for (unsigned i=0; i<format.size(); i++) {
    if (format[i] == '%' && i+1 < format.size()) {
      i++;
      char buf[32];
      switch (format[i]) {
      case '%':
	strcpy(buf, "%");
	break;
      case 'Y':
	sprintf(buf, "%.4d", dt->tm_year + 1900);
	break;
      case 'y':
	sprintf(buf, "%.2d", dt->tm_year % 100);
	break;
      case 'm':
	sprintf(buf, "%.2d", dt->tm_mon + 1);
	break;
      case 'd':
	sprintf(buf, "%.2d", dt->tm_mday);
	break;
      case 'H':
	sprintf(buf, "%.2d", dt->tm_hour);
	break;
      case 'I':
	sprintf(buf, "%.2d", (dt->tm_hour % 12) + 1);
	break;
      case 'k':
	sprintf(buf, "%d", dt->tm_hour);
	break;
      case 'l':
	sprintf(buf, "%d", (dt->tm_hour % 12) + 1);
	break;
      case 'M':
	sprintf(buf, "%.2d", dt->tm_min);
	break;

      case 'i':
	sprintf(buf, "%lld", (long long)bucket_id);
	break;
      case 'n':
	o += bucket_name;
	continue;
      default:
	// unknown code
	sprintf(buf, "%%%c", format[i]);
	break;
      }
      o += buf;
      continue;
    }
    o += format[i];
  }
  return o;
}

int rgw_log_op(struct req_state *s)
{
  struct rgw_log_entry entry;
  uint64_t bucket_id;

  if (!s->should_log)
    return 0;

  if (!s->bucket_name) {
    dout(5) << "nothing to log for operation" << dendl;
    return -EINVAL;
  }
  if (s->err.ret == -ERR_NO_SUCH_BUCKET) {
    if (!g_conf->rgw_log_nonexistent_bucket) {
      dout(5) << "bucket " << s->bucket << " doesn't exist, not logging" << dendl;
      return 0;
    }
    bucket_id = 0;
  } else {
    bucket_id = s->bucket.bucket_id;
  }
  entry.bucket = s->bucket_name;

  if (check_utf8(s->bucket_name, entry.bucket.size()) != 0) {
    dout(5) << "not logging op on bucket with non-utf8 name" << dendl;
    return 0;
  }

  if (s->object)
    entry.obj = s->object;
  else
    entry.obj = "-";

  entry.obj_size = s->obj_size;

  if (g_conf->rgw_remote_addr_param.length())
    set_param_str(s, g_conf->rgw_remote_addr_param.c_str(), entry.remote_addr);
  else
    set_param_str(s, "REMOTE_ADDR", entry.remote_addr);    
  set_param_str(s, "HTTP_USER_AGENT", entry.user_agent);
  set_param_str(s, "HTTP_REFERRER", entry.referrer);
  set_param_str(s, "REQUEST_URI", entry.uri);
  set_param_str(s, "REQUEST_METHOD", entry.op);

  entry.user = s->user.user_id;
  if (s->acl)
    entry.object_owner = s->acl->get_owner().get_id();
  entry.bucket_owner = s->bucket_owner;

  entry.time = s->time;
  entry.total_time = ceph_clock_now(g_ceph_context) - s->time;
  entry.bytes_sent = s->bytes_sent;
  entry.bytes_received = s->bytes_received;
  if (s->err.http_ret) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", s->err.http_ret);
    entry.http_status = buf;
  } else
    entry.http_status = "200"; // default

  entry.error_code = s->err.s3_code;
  entry.bucket_id = bucket_id;

  bufferlist bl;
  ::encode(entry, bl);

  struct tm bdt;
  time_t t = entry.time.sec();
  if (g_conf->rgw_log_object_name_utc)
    gmtime_r(&t, &bdt);
  else
    localtime_r(&t, &bdt);
  
  string oid = render_log_object_name(g_conf->rgw_log_object_name, &bdt,
				      s->bucket.bucket_id, entry.bucket.c_str());

  rgw_obj obj(log_bucket, oid);

  int ret = rgwstore->append_async(obj, bl.length(), bl);
  if (ret == -ENOENT) {
    string id;
    map<std::string, bufferlist> attrs;
    ret = rgwstore->create_bucket(id, log_bucket, attrs, true);
    if (ret < 0)
      goto done;
    // retry
    ret = rgwstore->append_async(obj, bl.length(), bl);
  }
done:
  if (ret < 0)
    dout(0) << "ERROR: failed to log entry" << dendl;

  return ret;
}

int rgw_log_intent(struct req_state *s, rgw_obj& obj, RGWIntentEvent intent)
{
  rgw_bucket intent_log_bucket(RGW_INTENT_LOG_POOL_NAME);

  rgw_intent_log_entry entry;
  entry.obj = obj;
  entry.intent = (uint32_t)intent;
  entry.op_time = s->time;

  struct tm bdt;
  time_t t = entry.op_time.sec();
  if (g_conf->rgw_intent_log_object_name_utc)
    gmtime_r(&t, &bdt);
  else
    localtime_r(&t, &bdt);

  char buf[obj.bucket.name.size() + 16];
  sprintf(buf, "%.4d-%.2d-%.2d-%lld-%s", (bdt.tm_year+1900), (bdt.tm_mon+1), bdt.tm_mday,
	  (long long)s->bucket.bucket_id, obj.bucket.name.c_str());
  string oid(buf);
  rgw_obj log_obj(intent_log_bucket, oid);

  bufferlist bl;
  ::encode(entry, bl);

  int ret = rgwstore->append_async(log_obj, bl.length(), bl);
  if (ret == -ENOENT) {
    string id;
    map<std::string, bufferlist> attrs;
    ret = rgwstore->create_bucket(id, intent_log_bucket, attrs, true);
    if (ret < 0)
      goto done;
    ret = rgwstore->append_async(log_obj, bl.length(), bl);
  }

done:
  return ret;
}
