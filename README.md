# ppshuai_async_redis_async
一个基于hiredis和libuv的异步请求访问封装库，可以轻松自由的在windows/linux下访问redis（支持集群）

# hiredis需要做如下改动：
```javascript
Parent: e777b0295eeeda89ee2ecef6ec5cb54889033d94 (Use correct flag in README.md)
Branch: master
Follows: v0.14.0
Precedes: 

    Commit libuv async request redis two problems and solutions.

------------------------------- adapters/libuv.h -------------------------------
index 39ef7cf..049096e 100644
@@ -84,7 +84,6 @@ static void redisLibuvCleanup(void *privdata) {
   uv_close((uv_handle_t*)&p->handle, on_close);
 }
 
-
 static int redisLibuvAttach(redisAsyncContext* ac, uv_loop_t* loop) {
   redisContext *c = &(ac->c);
 
@@ -106,7 +105,7 @@ static int redisLibuvAttach(redisAsyncContext* ac, uv_loop_t* loop) {
 
   memset(p, 0, sizeof(*p));
 
-  if (uv_poll_init(loop, &p->handle, c->fd) != 0) {
+  if (uv_poll_init_socket(loop, &p->handle, c->fd) != 0) {
     return REDIS_ERR;
   }
 

------------------------------------ net.c ------------------------------------
index e5f40b0..342d292 100644
@@ -478,6 +478,13 @@ addrretry:
         }
         if (blocking && redisSetBlocking(c,1) != REDIS_OK)
             goto error;
+
+#ifndef _MSC_VER
+		sleep(0);
+#else
+		_sleep(0);
+#endif // !_MSC_VER
+
         if (redisSetTcpNoDelay(c) != REDIS_OK)
             goto error;
 
```
