#pragma once

#include <list>
#include <mutex>
#include <thread>
#include <string>
#include <unordered_map>
#ifndef _MSC_VER
#include <condition_variable> 
#endif

#include "../ThirdParty/hiredis/hiredis/hiredis.h"
#include "../ThirdParty/hiredis/hiredis/async.h"
#include "../ThirdParty/hiredis/hiredis/adapters/libuv.h"

#if HIREDIS_MAJOR >= 0 && HIREDIS_MINOR >= 14 && HIREDIS_PATCH >= 0

#else
#define REDIS_REPLY_DOUBLE 7
#define REDIS_REPLY_BOOL 8
#define REDIS_REPLY_VERB 9
#define REDIS_REPLY_MAP 9
#define REDIS_REPLY_SET 10
#define REDIS_REPLY_ATTR 11
#define REDIS_REPLY_PUSH 12
#define REDIS_REPLY_BIGNUM 13
#endif

class CRedisAsyncLibuv;
struct redis_async_block {
	redis_async_block(const char* _ip,
		int _port,
		const char* _passwd = "",
		bool _cluster = false,
		int _slot = 0)
	{
		ip = _ip;
		port = _port;
		passwd = _passwd;
		cluster = _cluster;
		slot = _slot;
		c = nullptr;
		prauv = nullptr;
	}
	std::string ip;
	int port;
	std::string passwd;
	bool cluster;
	int slot;
	redisAsyncContext* c;
	CRedisAsyncLibuv* prauv;
};
struct redis_async_param {
	std::mutex m_action_lock;
	std::condition_variable m_action_cond;
	int status;
	std::string req;
	void* resp;
	void* error;
};

class CRedisAsyncLibuv {

#define T_OK	"OK"
#define L_OK	2
#define T_ASK	"ASK"
#define L_ASK	3
#define T_MOVED	"MOVED"
#define L_MOVED	5

private:
	int slot;
	bool cluster;
	uv_loop_t* loop;
	uv_async_t async;
	std::mutex locker;
	std::string passwd;
	std::shared_ptr<std::thread> redis_thread;
	std::unordered_map<std::string, struct redis_async_block> addr_map;
public:
	int get_slot() {
		return slot;
	}
	void set_slot(int _slot) {
		slot = _slot;
	}
	bool get_cluster() {
		return cluster;
	}
	const std::string& get_passwd()	{
		return passwd;
	}
	uv_loop_t* get_loop_ptr() {
		return loop;
	}
	void set_loop_ptr(uv_loop_t* _loop) {
		loop = _loop;
	}
	uv_async_t* get_async_ptr() {
		return &async;
	}
	std::unordered_map<std::string, struct redis_async_block>* get_addr_list() {
		return &addr_map;
	}
	void init_addr_list(const std::list<struct redis_async_block>& rab, const char* passwd = "", bool cluster = false, int slot = 0)
	{
		this->slot = slot;
		this->passwd = passwd;
		this->cluster = cluster;
		for (auto& it : rab)
		{
			std::string key = std::string(it.ip) + ":" + std::to_string(it.port);
			addr_map.insert({ key, it });
			addr_map.at(key).prauv = this;
			//addr_map.at(key).passwd = passwd;
			//addr_map.at(key).cluster = cluster;
			//addr_map.at(key).slot = slot;
		}
	}
	void close_cb(uv_handle_t* handle)
	{
		printf("close the async handle!\n");
	}

	void async_cb(uv_async_t* handle, int status)
	{
		printf("async_cb called!\n");
		uv_thread_t id = uv_thread_self();
		printf("thread id:%lu.\n", id);
		//uv_close((uv_handle_t*)&async, CRedisAsyncLibuv::close_cb);	//如果async没有关闭，消息队列是会阻塞的
	}
	void async_connect(struct redis_async_block * prab)
	{
		redisAsyncContext* c = redisAsyncConnect(prab->ip.c_str(), prab->port);
		if (c->err)
		{
			// Let *c leak for now...
			printf("Error: %s\n", c->errstr);
			return;
		}
		c->data = (void*)prab;

		redisLibuvAttach(c, this->get_loop_ptr());

		redisAsyncSetConnectCallback(c, [](const redisAsyncContext* c, int status) {
			struct redis_async_block* prab = reinterpret_cast<struct redis_async_block*>(c->data);
			if (status != REDIS_OK)
			{
				prab->c = nullptr;
				printf("Error: %s\n", c->errstr);
			}
			else
			{
				prab->c = (redisAsyncContext*)(c);
				printf("Connected...\n");
			}
			});
		redisAsyncSetDisconnectCallback(c, [](const redisAsyncContext* c, int status) {
			struct redis_async_block* prab = reinterpret_cast<struct redis_async_block*>(c->data);
			if (status != REDIS_OK)
			{
				printf("Error: %s\n", c->errstr);
			}
			else
			{
				printf("Disconnected...\n");
			}
			prab->c = nullptr;
			});
		if (this->get_passwd().length() > 0)
		{
			redisAsyncCommand((redisAsyncContext*)(c), [](redisAsyncContext* c, void* r, void* privdata) {
				redisReply* reply = reinterpret_cast<redisReply*>(r);
				printf("AUTH reply=%x\n", reply);
				if (reply == nullptr)
				{
					return;
				}
				printf("AUTH result: %d, %s\n", reply->type, reply->str);
				}, nullptr, "AUTH %s", this->get_passwd().c_str());
		}

		if (this->get_cluster() == false)
		{
			if (this->get_slot() < 0)
			{
				this->set_slot(0);
			}
			redisAsyncCommand((redisAsyncContext*)(c), [](redisAsyncContext* c, void* r, void* privdata) {
				struct redis_async_block* re = reinterpret_cast<struct redis_async_block*>(c->data);
				redisReply* reply = reinterpret_cast<redisReply*>(r);
				printf("select reply=%x\n", reply);
				if (reply == nullptr)
				{
					return;
				}
				printf("SELECT result: %d, %s\n", reply->type, reply->str);
				}, nullptr, ("SELECT " + std::to_string(this->get_slot())).c_str());
		}
	}
	void start_redis_loop_thread()
	{
		redis_thread = std::make_shared<std::thread>([](void* p) {
			CRedisAsyncLibuv* thiz = reinterpret_cast<CRedisAsyncLibuv*>(p);
			thiz->set_loop_ptr(uv_default_loop());
			/*
			uv_timer_t timer;
			uv_timer_init(loop, &timer);
			uv_timer_start(&timer, (uv_timer_cb)([](uv_timer_t* handle) {
				std::string error;
				std::string resp;
				redis_async_block* async_block_ptr = nullptr;
				for (auto& it : *reinterpret_cast<CRedisAsyncLibuv*>(g_async_redis_ptr)->get_addr_list())
				{
					if (it.second.c != nullptr)
					{
						async_block_ptr = &it.second;
						break;
					}
				}
				if (async_block_ptr != nullptr)
				{
					redisAsyncCommand((redisAsyncContext*)(async_block_ptr->c), [](redisAsyncContext* c, void* r, void* privdata) {
						redisReply* reply = reinterpret_cast<redisReply*>(r);
						printf("333GET reply=%x\n", reply);
						if (reply == nullptr)
						{
							return;
						}
						printf("333GET result: %d, %s\n", reply->type, reply->str);
						}, nullptr, "get foo");
				}
				}), 0, 1000);*/
			uv_async_init(thiz->get_loop_ptr(), thiz->get_async_ptr(), [](uv_async_t* handle) {
				;//printf("async_cb called!\n");
				/*uv_close((uv_handle_t*)handle, [](uv_handle_t* handle)
					{
						printf("close the async handle!\n");
					});*/
				});
			for (auto it = thiz->get_addr_list()->begin(); it != thiz->get_addr_list()->end(); it++)
			{
				thiz->async_connect(&it->second);
				/*redisAsyncContext* c = redisAsyncConnect(it->second.ip.c_str(), it->second.port);
				if (c->err)
				{
					// Let *c leak for now...
					printf("Error: %s\n", c->errstr);
					continue;
				}
				it->second.prauv = thiz;
				c->data = (void*)&it->second;

				redisLibuvAttach(c, thiz->get_loop_ptr());

				redisAsyncSetConnectCallback(c, [](const redisAsyncContext* c, int status) {
					struct redis_async_block* prab = reinterpret_cast<struct redis_async_block*>(c->data);
					if (status != REDIS_OK)
					{
						prab->c = nullptr;
						printf("Error: %s\n", c->errstr);
					}
					else
					{
						prab->c = (redisAsyncContext*)(c);
						printf("Connected...\n");
					}
					});
				redisAsyncSetDisconnectCallback(c, [](const redisAsyncContext* c, int status) {
					struct redis_async_block* prab = reinterpret_cast<struct redis_async_block*>(c->data);
					if (status != REDIS_OK)
					{
						printf("Error: %s\n", c->errstr);
					}
					else
					{
						printf("Disconnected...\n");
					}
					prab->c = nullptr;
					});
				if (it->second.passwd.length() > 0)
				{
					redisAsyncCommand((redisAsyncContext*)(c), [](redisAsyncContext* c, void* r, void* privdata) {
						redisReply* reply = reinterpret_cast<redisReply*>(r);
						printf("AUTH reply=%x\n", reply);
						if (reply == nullptr)
						{
							return;
						}
						printf("AUTH result: %d, %s\n", reply->type, reply->str);
						}, nullptr, "AUTH %s", it->second.passwd.c_str());
				}

				if (it->second.cluster == false)
				{
					if (it->second.slot < 0)
					{
						it->second.slot = 0;
					}
					redisAsyncCommand((redisAsyncContext*)(c), [](redisAsyncContext* c, void* r, void* privdata) {
						struct redis_async_block* re = reinterpret_cast<struct redis_async_block*>(c->data);
						redisReply* reply = reinterpret_cast<redisReply*>(r);
						printf("select reply=%x\n", reply);
						if (reply == nullptr)
						{
							return;
						}
						printf("SELECT result: %d, %s\n", reply->type, reply->str);
						}, nullptr, ("SELECT " + std::to_string(it->second.slot)).c_str());
				}*/
			}
			uv_run(thiz->get_loop_ptr(), UV_RUN_DEFAULT);
			}, this);
	}
	void stop_redis_loop_thread()
	{
		if (redis_thread != nullptr)
		{
			if (redis_thread->joinable())
			{
				for (auto& it : addr_map)
				{
					if (it.second.c != nullptr)
					{
						redisAsyncCommand(it.second.c, [](redisAsyncContext* c, void* r, void* privdata) {
							// Disconnect after receiving the reply to GET
							redisAsyncDisconnect(c);
							}, nullptr, "GET NIL");
						uv_async_send(this->get_async_ptr());
					}
				}
				uv_close((uv_handle_t*)this->get_async_ptr(), [](uv_handle_t* handle)
					{
						printf("close the async handle!\n");
					});
				redis_thread->join();
			}
		}
	}

public:
	////////////////////////////////////////////////////
	// execute command return status(exp: set/hset)
	// return: 0-success, !0-failure
	int redis_async_execute(std::string& error, const std::string& cmd)
	{
		struct redis_async_param rap;
		redis_async_block* async_block_ptr = nullptr;
		for (auto& it : addr_map)
		{
			if (it.second.c != nullptr)
			{
				async_block_ptr = &it.second;
				break;
			}
		}
		if (async_block_ptr != nullptr)
		{
			locker.lock();
			rap.status = 1;
			rap.req = cmd;
			rap.error = reinterpret_cast<void*>(&error);

			redisAsyncCommand(async_block_ptr->c, [](redisAsyncContext* c, void* r, void* privdata) {
				struct redis_async_block* prab = reinterpret_cast<struct redis_async_block*>(c->data);
				struct redis_async_param* prap = reinterpret_cast<struct redis_async_param*>(privdata);
				redisReply* reply = reinterpret_cast<redisReply*>(r);
				if (reply == nullptr)
				{
					prap->status = (-1);
					reinterpret_cast<std::string*>(prap->error)->assign("reply=nullptr");
					prap->m_action_cond.notify_one();
					return;
				}
				if (reply->type != REDIS_REPLY_ERROR)
				{
					switch (reply->type)
					{
					case REDIS_REPLY_STRING:// 1
					{
						prap->status = (-1);
						reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_STRING");
					}
					break;
					case REDIS_REPLY_ARRAY:// 2
					{
						prap->status = (-1);
						reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_ARRAY");
					}
					break;
					case  REDIS_REPLY_INTEGER:// 3
					{
						prap->status = (-1);
						reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_INTEGER");
					}
					break;
					case  REDIS_REPLY_NIL:// 4
					{
						prap->status = (-1);
						reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_NIL");
					}
					break;
					case REDIS_REPLY_STATUS:// 5
					{
						reinterpret_cast<std::string*>(prap->error)->assign(reply->str);
						if (reinterpret_cast<std::string*>(prap->error)->compare(0, L_OK, T_OK) == 0)
						{
							prap->status = (0);
						}
						else
						{
							prap->status = (-1);
						}
						reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_STATUS");
					}
					break;
					case REDIS_REPLY_DOUBLE:// 7
					{
						prap->status = (-1);
						reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_DOUBLE");
					}
					break;
					case REDIS_REPLY_BOOL:// 8
					{
						prap->status = (-1);
						reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_BOOL");
					}
					break;
					//case REDIS_REPLY_VERB:// 9
					case REDIS_REPLY_MAP:// 9
					{
						prap->status = (-1);
						reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_MAP/REDIS_REPLY_VERB");
					}
					break;
					case REDIS_REPLY_SET:// 10
					{
						prap->status = (-1);
						reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_SET");
					}
					break;
					case REDIS_REPLY_ATTR:// 11
					{
						prap->status = (-1);
						reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_ATTR");
					}
					break;
					case REDIS_REPLY_PUSH:// 12
					{
						prap->status = (-1);
						reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_PUSH");
					}
					break;
					case REDIS_REPLY_BIGNUM:// 13
					{
						prap->status = (-1);
						reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_BIGNUM");
					}
					break;
					default:
					{
						prap->status = (-1);
						reinterpret_cast<std::string*>(prap->error)->assign("reply->type unknown=" + std::to_string(reply->type));
					}
					break;
					}
					prap->m_action_cond.notify_one();
					return;
				}
				else
				{
					if (reply->str != nullptr)
					{
						reinterpret_cast<std::string*>(prap->error)->assign(reply->str);
						if (((reinterpret_cast<std::string*>(prap->error)->length() > L_MOVED)
							&& (reinterpret_cast<std::string*>(prap->error)->compare(0, L_MOVED, T_MOVED) == 0))
							|| ((reinterpret_cast<std::string*>(prap->error)->length() > L_ASK)
								&& (reinterpret_cast<std::string*>(prap->error)->compare(0, L_ASK, T_ASK) == 0)))
						{
							std::string node_key = reinterpret_cast<std::string*>(prap->error)->substr(reinterpret_cast<std::string*>(prap->error)->rfind(" ") + 1);
							/*if (prab->prauv->get_addr_list()->find(node_key) == prab->prauv->get_addr_list()->end())
							{
								std::string ip = node_key.substr(0, node_key.find(":")).c_str();
								int port = std::stoi(node_key.substr(node_key.find(":") + 1));
								redis_async_block rab_tmp(ip.c_str(), port);
								prab->prauv->get_addr_list()->insert(std::unordered_map<std::string, struct redis_async_block>::value_type(node_key, rab_tmp));
								prab->prauv->async_connect(&prab->prauv->get_addr_list()->at(node_key));
							}*/
							redisAsyncCommand(prab->prauv->get_addr_list()->at(node_key).c, [](redisAsyncContext* c, void* r, void* privdata) {
								struct redis_async_block* prab = reinterpret_cast<struct redis_async_block*>(c->data);
								struct redis_async_param* prap = reinterpret_cast<struct redis_async_param*>(privdata);
								redisReply* reply = reinterpret_cast<redisReply*>(r);
								if (reply == nullptr)
								{
									prap->status = (-1);
									reinterpret_cast<std::string*>(prap->error)->assign("reply=nullptr");
									prap->m_action_cond.notify_one();
									return;
								}
								if (reply->type != REDIS_REPLY_ERROR)
								{
									switch (reply->type)
									{
									case REDIS_REPLY_STRING:// 1
									{
										prap->status = (-1);
										reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_STRING");
									}
									break;
									case REDIS_REPLY_ARRAY:// 2
									{
										prap->status = (-1);
										reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_ARRAY");
									}
									break;
									case  REDIS_REPLY_INTEGER:// 3
									{
										prap->status = (-1);
										reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_INTEGER");
									}
									break;
									case  REDIS_REPLY_NIL:// 4
									{
										prap->status = (-1);
										reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_NIL");
									}
									break;
									case REDIS_REPLY_STATUS:// 5
									{
										reinterpret_cast<std::string*>(prap->error)->assign(reply->str);
										if (reinterpret_cast<std::string*>(prap->error)->compare(0, L_OK, T_OK) == 0)
										{
											prap->status = (0);
										}
										else
										{
											prap->status = (-1);
										}
										reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_STATUS");
									}
									break;
									case REDIS_REPLY_DOUBLE:// 7
									{
										prap->status = (-1);
										reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_DOUBLE");
									}
									break;
									case REDIS_REPLY_BOOL:// 8
									{
										prap->status = (-1);
										reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_BOOL");
									}
									break;
									//case REDIS_REPLY_VERB:// 9
									case REDIS_REPLY_MAP:// 9
									{
										prap->status = (-1);
										reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_MAP/REDIS_REPLY_VERB");
									}
									break;
									case REDIS_REPLY_SET:// 10
									{
										prap->status = (-1);
										reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_SET");
									}
									break;
									case REDIS_REPLY_ATTR:// 11
									{
										prap->status = (-1);
										reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_ATTR");
									}
									break;
									case REDIS_REPLY_PUSH:// 12
									{
										prap->status = (-1);
										reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_PUSH");
									}
									break;
									case REDIS_REPLY_BIGNUM:// 13
									{
										prap->status = (-1);
										reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_BIGNUM");
									}
									break;
									default:
									{
										prap->status = (-1);
										reinterpret_cast<std::string*>(prap->error)->assign("reply->type unknown=" + std::to_string(reply->type));
									}
									break;
									}
									prap->m_action_cond.notify_one();
									return;
								}
								else
								{
									if (reply->str != nullptr)
									{
										prap->status = (-1);
										reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_ERROR, reply->str=" + std::string(reply->str));
										prap->m_action_cond.notify_one();
										return;
									}
									else
									{
										prap->status = (-1);
										reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_ERROR, reply->str=nullptr");
										prap->m_action_cond.notify_one();
										return;
									}
								}
								}, prap, prap->req.c_str());
							uv_async_send(prab->prauv->get_async_ptr());
						}
						else
						{
							prap->status = (-1);
							reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_ERROR, reply->str=" + std::string(reply->str));
							prap->m_action_cond.notify_one();
							return;
						}
					}
					else
					{
						prap->status = (-1);
						reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_ERROR, reply->str=nullptr");
						prap->m_action_cond.notify_one();
						return;
					}
				}

				}, (void*)&rap, rap.req.c_str());

			uv_async_send(this->get_async_ptr());
			std::unique_lock<std::mutex>  ulock(rap.m_action_lock);
			while (rap.status == 1)
			{
				rap.m_action_cond.wait(ulock);
			}
			ulock.unlock();
			locker.unlock();
		}
		else
		{
			rap.status = (-1);
			error.assign("No connected redis instance");
		}
		if (rap.status != 0)
		{
			printf("[%ld]error=%s\n", std::this_thread::get_id(), error.c_str());
		}
		else
		{
			printf("[%ld]\n", std::this_thread::get_id());
		}
		return rap.status;
	}
	////////////////////////////////////////////////////
	// execute command return string
	// return: 0-success, !0-failure
	int redis_async_execute(std::string& resp, std::string& error, const std::string& cmd)
	{
		struct redis_async_param rap;
		redis_async_block* async_block_ptr = nullptr;
		for (auto& it : addr_map)
		{
			if (it.second.c != nullptr)
			{
				async_block_ptr = &it.second;
				break;
			}
		}
		if (async_block_ptr != nullptr)
		{
			locker.lock();
			rap.status = 1;
			rap.req = cmd;
			rap.resp = reinterpret_cast<void*>(&resp);
			rap.error = reinterpret_cast<void*>(&error);
			redisAsyncCommand(async_block_ptr->c, [](redisAsyncContext* c, void* r, void* privdata) {
				struct redis_async_block* prab = reinterpret_cast<struct redis_async_block*>(c->data);
				struct redis_async_param* prap = reinterpret_cast<struct redis_async_param*>(privdata);
				redisReply* reply = reinterpret_cast<redisReply*>(r);
				if (reply == nullptr)
				{
					prap->status = (-1);
					reinterpret_cast<std::string*>(prap->error)->assign("reply=nullptr");
					prap->m_action_cond.notify_one();
					return;
				}
				if (reply->type != REDIS_REPLY_ERROR)
				{
					switch (reply->type)
					{
					case REDIS_REPLY_STRING:// 1
					{
						prap->status = (0);
						reinterpret_cast<std::string*>(prap->resp)->assign(reply->str);
						reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_STRING");
					}
					break;
					case REDIS_REPLY_ARRAY:// 2
					{
						prap->status = (-1);
						reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_ARRAY");
					}
					break;
					case  REDIS_REPLY_INTEGER:// 3
					{
						prap->status = (-1);
						reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_INTEGER");
					}
					break;
					case  REDIS_REPLY_NIL:// 4
					{
						prap->status = (-1);
						reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_NIL");
					}
					break;
					case REDIS_REPLY_STATUS:// 5
					{
						prap->status = (-1);
						reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_STATUS");
					}
					break;
					case REDIS_REPLY_DOUBLE:// 7
					{
						prap->status = (-1);
						reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_DOUBLE");
					}
					break;
					case REDIS_REPLY_BOOL:// 8
					{
						prap->status = (-1);
						reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_BOOL");
					}
					break;
					//case REDIS_REPLY_VERB:// 9
					case REDIS_REPLY_MAP:// 9
					{
						prap->status = (-1);
						reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_MAP/REDIS_REPLY_VERB");
					}
					break;
					case REDIS_REPLY_SET:// 10
					{
						prap->status = (-1);
						reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_SET");
					}
					break;
					case REDIS_REPLY_ATTR:// 11
					{
						prap->status = (-1);
						reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_ATTR");
					}
					break;
					case REDIS_REPLY_PUSH:// 12
					{
						prap->status = (-1);
						reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_PUSH");
					}
					break;
					case REDIS_REPLY_BIGNUM:// 13
					{
						prap->status = (-1);
						reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_BIGNUM");
					}
					break;
					default:
					{
						prap->status = (-1);
						reinterpret_cast<std::string*>(prap->error)->assign("reply->type unknown=" + std::to_string(reply->type));
					}
					break;
					}
					prap->m_action_cond.notify_one();
					return;
				}
				else
				{
					if (reply->str != nullptr)
					{
						reinterpret_cast<std::string*>(prap->error)->assign(reply->str);
						if (((reinterpret_cast<std::string*>(prap->error)->length() > L_MOVED)
							&& (reinterpret_cast<std::string*>(prap->error)->compare(0, L_MOVED, T_MOVED) == 0))
							|| ((reinterpret_cast<std::string*>(prap->error)->length() > L_ASK)
								&& (reinterpret_cast<std::string*>(prap->error)->compare(0, L_ASK, T_ASK) == 0)))
						{
							std::string node_key = reinterpret_cast<std::string*>(prap->error)->substr(reinterpret_cast<std::string*>(prap->error)->rfind(" ") + 1);
							/*if (prab->prauv->get_addr_list()->find(node_key) == prab->prauv->get_addr_list()->end())
							{
								redis_async_block rab_tmp(node_key.substr(0, node_key.find(":")).c_str(), std::stoi(node_key.substr(node_key.find(":") + 1)));
								prab->prauv->get_addr_list()->insert(std::unordered_map<std::string, struct redis_async_block>::value_type(node_key, rab_tmp));
								prab->prauv->async_connect(&prab->prauv->get_addr_list()->at(node_key));
							}*/
							redisAsyncCommand(prab->prauv->get_addr_list()->at(node_key).c, [](redisAsyncContext* c, void* r, void* privdata) {
								struct redis_async_block* prab = reinterpret_cast<struct redis_async_block*>(c->data);
								struct redis_async_param* prap = reinterpret_cast<struct redis_async_param*>(privdata);
								redisReply* reply = reinterpret_cast<redisReply*>(r);
								if (reply == nullptr)
								{
									prap->status = (-1);
									reinterpret_cast<std::string*>(prap->error)->assign("reply=nullptr");
									prap->m_action_cond.notify_one();
									return;
								}
								if (reply->type != REDIS_REPLY_ERROR)
								{
									switch (reply->type)
									{
									case REDIS_REPLY_STRING:// 1
									{
										prap->status = (0);
										reinterpret_cast<std::string*>(prap->resp)->assign(reply->str);
										reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_STRING");
									}
									break;
									case REDIS_REPLY_ARRAY:// 2
									{
										prap->status = (-1);
										reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_ARRAY");
									}
									break;
									case  REDIS_REPLY_INTEGER:// 3
									{
										prap->status = (-1);
										reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_INTEGER");
									}
									break;
									case  REDIS_REPLY_NIL:// 4
									{
										prap->status = (-1);
										reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_NIL");
									}
									break;
									case REDIS_REPLY_STATUS:// 5
									{
										prap->status = (-1);
										reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_STATUS");
									}
									break;
									case REDIS_REPLY_DOUBLE:// 7
									{
										prap->status = (-1);
										reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_DOUBLE");
									}
									break;
									case REDIS_REPLY_BOOL:// 8
									{
										prap->status = (-1);
										reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_BOOL");
									}
									break;
									//case REDIS_REPLY_VERB:// 9
									case REDIS_REPLY_MAP:// 9
									{
										prap->status = (-1);
										reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_MAP/REDIS_REPLY_VERB");
									}
									break;
									case REDIS_REPLY_SET:// 10
									{
										prap->status = (-1);
										reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_SET");
									}
									break;
									case REDIS_REPLY_ATTR:// 11
									{
										prap->status = (-1);
										reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_ATTR");
									}
									break;
									case REDIS_REPLY_PUSH:// 12
									{
										prap->status = (-1);
										reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_PUSH");
									}
									break;
									case REDIS_REPLY_BIGNUM:// 13
									{
										prap->status = (-1);
										reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_BIGNUM");
									}
									break;
									default:
									{
										prap->status = (-1);
										reinterpret_cast<std::string*>(prap->error)->assign("reply->type unknown=" + std::to_string(reply->type));
									}
									break;
									}
									prap->m_action_cond.notify_one();
									return;
								}
								else
								{
									if (reply->str != nullptr)
									{
										prap->status = (-1);
										reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_ERROR, reply->str=" + std::string(reply->str));
										prap->m_action_cond.notify_one();
										return;
									}
									else
									{
										prap->status = (-1);
										reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_ERROR, reply->str=nullptr");
										prap->m_action_cond.notify_one();
										return;
									}
								}
								}, prap, prap->req.c_str());
							uv_async_send(prab->prauv->get_async_ptr());
						}
						else
						{
							prap->status = (-1);
							reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_ERROR, reply->str=" + std::string(reply->str));
							prap->m_action_cond.notify_one();
							return;
						}
					}
					else
					{
						prap->status = (-1);
						reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_ERROR, reply->str=nullptr");
						prap->m_action_cond.notify_one();
						return;
					}
				}

				}, (void*)&rap, rap.req.c_str());
			uv_async_send(this->get_async_ptr());
			std::unique_lock<std::mutex> ulock(rap.m_action_lock);
			while (rap.status == 1)
			{
				rap.m_action_cond.wait(ulock);
			}
			ulock.unlock();
			locker.unlock();
		}
		else
		{
			rap.status = (-1);
			error.assign("No connected redis instance");
		}
		if (rap.status != 0)
		{
			printf("[%ld]error=%s, result=%s\n", std::this_thread::get_id(), error.c_str(), resp.c_str());
		}
		else
		{
			printf("[%ld]result=%s\n", std::this_thread::get_id(), resp.c_str());
		}
		return rap.status;
	}
	////////////////////////////////////////////////////
	// execute command return integer
	// return: 0-success, !0-failure
	int redis_async_execute(int64_t& resp, std::string& error, const std::string& cmd)
	{
		struct redis_async_param rap;
		redis_async_block* async_block_ptr = nullptr;
		for (auto& it : addr_map)
		{
			if (it.second.c != nullptr)
			{
				async_block_ptr = &it.second;
				break;
			}
		}
		if (async_block_ptr != nullptr)
		{
			locker.lock();
			rap.status = 1;
			rap.req = cmd;
			rap.resp = reinterpret_cast<void*>(&resp);
			rap.error = reinterpret_cast<void*>(&error);

			redisAsyncCommand(async_block_ptr->c, [](redisAsyncContext* c, void* r, void* privdata) {
				struct redis_async_block* prab = reinterpret_cast<struct redis_async_block*>(c->data);
				struct redis_async_param* prap = reinterpret_cast<struct redis_async_param*>(privdata);
				redisReply* reply = reinterpret_cast<redisReply*>(r);
				if (reply == nullptr)
				{
					prap->status = (-1);
					reinterpret_cast<std::string*>(prap->error)->assign("reply=nullptr");
					prap->m_action_cond.notify_one();
					return;
				}
				if (reply->type != REDIS_REPLY_ERROR)
				{
					switch (reply->type)
					{
					case REDIS_REPLY_STRING:// 1
					{
						prap->status = (-1);
						reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_STRING");
					}
					break;
					case REDIS_REPLY_ARRAY:// 2
					{
						prap->status = (-1);
						reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_ARRAY");
					}
					break;
					case  REDIS_REPLY_INTEGER:// 3
					{
						prap->status = (0);
						*(reinterpret_cast<int64_t*>(prap->resp)) = reply->integer;
						reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_INTEGER");
					}
					break;
					case  REDIS_REPLY_NIL:// 4
					{
						prap->status = (-1);
						reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_NIL");
					}
					break;
					case REDIS_REPLY_STATUS:// 5
					{
						prap->status = (-1);
						reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_STATUS");
					}
					break;
					case REDIS_REPLY_DOUBLE:// 7
					{
						prap->status = (-1);
						reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_DOUBLE");
					}
					break;
					case REDIS_REPLY_BOOL:// 8
					{
						prap->status = (-1);
						reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_BOOL");
					}
					break;
					//case REDIS_REPLY_VERB:// 9
					case REDIS_REPLY_MAP:// 9
					{
						prap->status = (-1);
						reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_MAP/REDIS_REPLY_VERB");
					}
					break;
					case REDIS_REPLY_SET:// 10
					{
						prap->status = (-1);
						reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_SET");
					}
					break;
					case REDIS_REPLY_ATTR:// 11
					{
						prap->status = (-1);
						reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_ATTR");
					}
					break;
					case REDIS_REPLY_PUSH:// 12
					{
						prap->status = (-1);
						reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_PUSH");
					}
					break;
					case REDIS_REPLY_BIGNUM:// 13
					{
						prap->status = (-1);
						reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_BIGNUM");
					}
					break;
					default:
					{
						prap->status = (-1);
						reinterpret_cast<std::string*>(prap->error)->assign("reply->type unknown=" + std::to_string(reply->type));
					}
					break;
					}
					prap->m_action_cond.notify_one();
					return;
				}
				else
				{
					if (reply->str != nullptr)
					{
						reinterpret_cast<std::string*>(prap->error)->assign(reply->str);
						if (((reinterpret_cast<std::string*>(prap->error)->length() > L_MOVED)
							&& (reinterpret_cast<std::string*>(prap->error)->compare(0, L_MOVED, T_MOVED) == 0))
							|| ((reinterpret_cast<std::string*>(prap->error)->length() > L_ASK)
								&& (reinterpret_cast<std::string*>(prap->error)->compare(0, L_ASK, T_ASK) == 0)))
						{
							std::string node_key = reinterpret_cast<std::string*>(prap->error)->substr(reinterpret_cast<std::string*>(prap->error)->rfind(" ") + 1);
							redisAsyncCommand(prab->prauv->get_addr_list()->at(node_key).c, [](redisAsyncContext* c, void* r, void* privdata) {
								struct redis_async_block* prab = reinterpret_cast<struct redis_async_block*>(c->data);
								struct redis_async_param* prap = reinterpret_cast<struct redis_async_param*>(privdata);
								redisReply* reply = reinterpret_cast<redisReply*>(r);
								if (reply == nullptr)
								{
									prap->status = (-1);
									reinterpret_cast<std::string*>(prap->error)->assign("reply=nullptr");
									prap->m_action_cond.notify_one();
									return;
								}
								if (reply->type != REDIS_REPLY_ERROR)
								{
									switch (reply->type)
									{
									case REDIS_REPLY_STRING:// 1
									{
										prap->status = (-1);
										reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_STRING");
									}
									break;
									case REDIS_REPLY_ARRAY:// 2
									{
										prap->status = (-1);
										reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_ARRAY");
									}
									break;
									case  REDIS_REPLY_INTEGER:// 3
									{
										prap->status = (0);
										*(reinterpret_cast<int64_t*>(prap->resp)) = reply->integer;
										reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_INTEGER");
									}
									break;
									case  REDIS_REPLY_NIL:// 4
									{
										prap->status = (-1);
										reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_NIL");
									}
									break;
									case REDIS_REPLY_STATUS:// 5
									{
										prap->status = (-1);
										reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_STATUS");
									}
									break;
									case REDIS_REPLY_DOUBLE:// 7
									{
										prap->status = (-1);
										reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_DOUBLE");
									}
									break;
									case REDIS_REPLY_BOOL:// 8
									{
										prap->status = (-1);
										reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_BOOL");
									}
									break;
									//case REDIS_REPLY_VERB:// 9
									case REDIS_REPLY_MAP:// 9
									{
										prap->status = (-1);
										reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_MAP/REDIS_REPLY_VERB");
									}
									break;
									case REDIS_REPLY_SET:// 10
									{
										prap->status = (-1);
										reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_SET");
									}
									break;
									case REDIS_REPLY_ATTR:// 11
									{
										prap->status = (-1);
										reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_ATTR");
									}
									break;
									case REDIS_REPLY_PUSH:// 12
									{
										prap->status = (-1);
										reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_PUSH");
									}
									break;
									case REDIS_REPLY_BIGNUM:// 13
									{
										prap->status = (-1);
										reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_BIGNUM");
									}
									break;
									default:
									{
										prap->status = (-1);
										reinterpret_cast<std::string*>(prap->error)->assign("reply->type unknown=" + std::to_string(reply->type));
									}
									break;
									}
									prap->m_action_cond.notify_one();
									return;
								}
								else
								{
									if (reply->str != nullptr)
									{
										prap->status = (-1);
										reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_ERROR, reply->str=" + std::string(reply->str));
										prap->m_action_cond.notify_one();
										return;
									}
									else
									{
										prap->status = (-1);
										reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_ERROR, reply->str=nullptr");
										prap->m_action_cond.notify_one();
										return;
									}
								}
								}, prap, prap->req.c_str());
							uv_async_send(prab->prauv->get_async_ptr());
						}
						else
						{
							prap->status = (-1);
							reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_ERROR, reply->str=" + std::string(reply->str));
							prap->m_action_cond.notify_one();
							return;
						}
					}
					else
					{
						prap->status = (-1);
						reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_ERROR, reply->str=nullptr");
						prap->m_action_cond.notify_one();
						return;
					}
				}

				}, (void*)&rap, rap.req.c_str());
			uv_async_send(this->get_async_ptr());
			std::unique_lock<std::mutex>  ulock(rap.m_action_lock);
			while (rap.status == 1)
			{
				rap.m_action_cond.wait(ulock);
			}
			ulock.unlock();
			locker.unlock();
		}
		else
		{
			rap.status = (-1);
			error.assign("No connected redis instance");
		}
		if (rap.status != 0)
		{
			printf("[%ld]error=%s, result=%lld\n", std::this_thread::get_id(), error.c_str(), resp);
		}
		else
		{
			printf("[%ld]result=%lld\n", std::this_thread::get_id(), resp);
		}
		return rap.status;
	}
	////////////////////////////////////////////////////
	// execute command return double
	// return: 0-success, !0-failure
	int redis_async_execute(double& resp, std::string& error, const std::string& cmd)
	{
		struct redis_async_param rap;
		redis_async_block* async_block_ptr = nullptr;
		for (auto& it : addr_map)
		{
			if (it.second.c != nullptr)
			{
				async_block_ptr = &it.second;
				break;
			}
		}
		if (async_block_ptr != nullptr)
		{
			locker.lock();
			rap.status = 1;
			rap.req = cmd;
			rap.resp = reinterpret_cast<void*>(&resp);
			rap.error = reinterpret_cast<void*>(&error);

			redisAsyncCommand(async_block_ptr->c, [](redisAsyncContext* c, void* r, void* privdata) {
				struct redis_async_block* prab = reinterpret_cast<struct redis_async_block*>(c->data);
				struct redis_async_param* prap = reinterpret_cast<struct redis_async_param*>(privdata);
				redisReply* reply = reinterpret_cast<redisReply*>(r);
				if (reply == nullptr)
				{
					prap->status = (-1);
					reinterpret_cast<std::string*>(prap->error)->assign("reply=nullptr");
					prap->m_action_cond.notify_one();
					return;
				}
				if (reply->type != REDIS_REPLY_ERROR)
				{
					switch (reply->type)
					{
					case REDIS_REPLY_STRING:// 1
					{
						prap->status = (-1);
						reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_STRING");
					}
					break;
					case REDIS_REPLY_ARRAY:// 2
					{
						prap->status = (-1);
						reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_ARRAY");
					}
					break;
					case  REDIS_REPLY_INTEGER:// 3
					{
						prap->status = (-1);
						reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_INTEGER");
					}
					break;
					case  REDIS_REPLY_NIL:// 4
					{
						prap->status = (-1);
						reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_NIL");
					}
					break;
					case REDIS_REPLY_STATUS:// 5
					{
						prap->status = (-1);
						reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_STATUS");
					}
					break;
					case REDIS_REPLY_DOUBLE:// 7
					{
						prap->status = (0);
						memcpy(reinterpret_cast<double*>(prap->resp), &reply->integer, sizeof(double));
						reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_DOUBLE");
					}
					break;
					case REDIS_REPLY_BOOL:// 8
					{
						prap->status = (-1);
						reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_BOOL");
					}
					break;
					//case REDIS_REPLY_VERB:// 9
					case REDIS_REPLY_MAP:// 9
					{
						prap->status = (-1);
						reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_MAP/REDIS_REPLY_VERB");
					}
					break;
					case REDIS_REPLY_SET:// 10
					{
						prap->status = (-1);
						reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_SET");
					}
					break;
					case REDIS_REPLY_ATTR:// 11
					{
						prap->status = (-1);
						reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_ATTR");
					}
					break;
					case REDIS_REPLY_PUSH:// 12
					{
						prap->status = (-1);
						reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_PUSH");
					}
					break;
					case REDIS_REPLY_BIGNUM:// 13
					{
						prap->status = (-1);
						reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_BIGNUM");
					}
					break;
					default:
					{
						prap->status = (-1);
						reinterpret_cast<std::string*>(prap->error)->assign("reply->type unknown=" + std::to_string(reply->type));
					}
					break;
					}
					prap->m_action_cond.notify_one();
					return;
				}
				else
				{
					if (reply->str != nullptr)
					{
						reinterpret_cast<std::string*>(prap->error)->assign(reply->str);
						if (((reinterpret_cast<std::string*>(prap->error)->length() > L_MOVED)
							&& (reinterpret_cast<std::string*>(prap->error)->compare(0, L_MOVED, T_MOVED) == 0))
							|| ((reinterpret_cast<std::string*>(prap->error)->length() > L_ASK)
								&& (reinterpret_cast<std::string*>(prap->error)->compare(0, L_ASK, T_ASK) == 0)))
						{
							std::string node_key = reinterpret_cast<std::string*>(prap->error)->substr(reinterpret_cast<std::string*>(prap->error)->rfind(" ") + 1);
							redisAsyncCommand(prab->prauv->get_addr_list()->at(node_key).c, [](redisAsyncContext* c, void* r, void* privdata) {
								struct redis_async_block* prab = reinterpret_cast<struct redis_async_block*>(c->data);
								struct redis_async_param* prap = reinterpret_cast<struct redis_async_param*>(privdata);
								redisReply* reply = reinterpret_cast<redisReply*>(r);
								if (reply == nullptr)
								{
									prap->status = (-1);
									reinterpret_cast<std::string*>(prap->error)->assign("reply=nullptr");
									prap->m_action_cond.notify_one();
									return;
								}
								if (reply->type != REDIS_REPLY_ERROR)
								{
									switch (reply->type)
									{
									case REDIS_REPLY_STRING:// 1
									{
										prap->status = (-1);
										reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_STRING");
									}
									break;
									case REDIS_REPLY_ARRAY:// 2
									{
										prap->status = (-1);
										reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_ARRAY");
									}
									break;
									case  REDIS_REPLY_INTEGER:// 3
									{
										prap->status = (-1);
										reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_INTEGER");
									}
									break;
									case  REDIS_REPLY_NIL:// 4
									{
										prap->status = (-1);
										reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_NIL");
									}
									break;
									case REDIS_REPLY_STATUS:// 5
									{
										prap->status = (-1);
										reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_STATUS");
									}
									break;
									case REDIS_REPLY_DOUBLE:// 7
									{
										prap->status = (0);
										memcpy(reinterpret_cast<double*>(prap->resp), &reply->integer, sizeof(double));
										reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_DOUBLE");
									}
									break;
									case REDIS_REPLY_BOOL:// 8
									{
										prap->status = (-1);
										reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_BOOL");
									}
									break;
									//case REDIS_REPLY_VERB:// 9
									case REDIS_REPLY_MAP:// 9
									{
										prap->status = (-1);
										reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_MAP/REDIS_REPLY_VERB");
									}
									break;
									case REDIS_REPLY_SET:// 10
									{
										prap->status = (-1);
										reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_SET");
									}
									break;
									case REDIS_REPLY_ATTR:// 11
									{
										prap->status = (-1);
										reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_ATTR");
									}
									break;
									case REDIS_REPLY_PUSH:// 12
									{
										prap->status = (-1);
										reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_PUSH");
									}
									break;
									case REDIS_REPLY_BIGNUM:// 13
									{
										prap->status = (-1);
										reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_BIGNUM");
									}
									break;
									default:
									{
										prap->status = (-1);
										reinterpret_cast<std::string*>(prap->error)->assign("reply->type unknown=" + std::to_string(reply->type));
									}
									break;
									}
									prap->m_action_cond.notify_one();
									return;
								}
								else
								{
									if (reply->str != nullptr)
									{
										prap->status = (-1);
										reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_ERROR, reply->str=" + std::string(reply->str));
										prap->m_action_cond.notify_one();
										return;
									}
									else
									{
										prap->status = (-1);
										reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_ERROR, reply->str=nullptr");
										prap->m_action_cond.notify_one();
										return;
									}
								}
								}, prap, prap->req.c_str());
							uv_async_send(prab->prauv->get_async_ptr());
						}
						else
						{
							prap->status = (-1);
							reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_ERROR, reply->str=" + std::string(reply->str));
							prap->m_action_cond.notify_one();
							return;
						}
					}
					else
					{
						prap->status = (-1);
						reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_ERROR, reply->str=nullptr");
						prap->m_action_cond.notify_one();
						return;
					}
				}

				}, (void*)&rap, rap.req.c_str());
			uv_async_send(this->get_async_ptr());
			std::unique_lock<std::mutex>  ulock(rap.m_action_lock);
			while (rap.status == 1)
			{
				rap.m_action_cond.wait(ulock);
			}
			ulock.unlock();
			locker.unlock();
		}
		else
		{
			rap.status = (-1);
			error.assign("No connected redis instance");
		}
		if (rap.status != 0)
		{
			printf("[%ld]error=%s, result=%lld\n", std::this_thread::get_id(), error.c_str(), resp);
		}
		else
		{
			printf("[%ld]result=%lld\n", std::this_thread::get_id(), resp);
		}
		return rap.status;
	}

	////////////////////////////////////////////////////
	// execute command return double
	// return: 0-success, !0-failure
	int redis_async_execute(bool& resp, std::string& error, const std::string& cmd)
	{
		struct redis_async_param rap;
		redis_async_block* async_block_ptr = nullptr;
		for (auto& it : addr_map)
		{
			if (it.second.c != nullptr)
			{
				async_block_ptr = &it.second;
				break;
			}
		}
		if (async_block_ptr != nullptr)
		{
			locker.lock();
			rap.status = 1;
			rap.req = cmd;
			rap.resp = reinterpret_cast<void*>(&resp);
			rap.error = reinterpret_cast<void*>(&error);

			redisAsyncCommand(async_block_ptr->c, [](redisAsyncContext* c, void* r, void* privdata) {
				struct redis_async_block* prab = reinterpret_cast<struct redis_async_block*>(c->data);
				struct redis_async_param* prap = reinterpret_cast<struct redis_async_param*>(privdata);
				redisReply* reply = reinterpret_cast<redisReply*>(r);
				if (reply == nullptr)
				{
					prap->status = (-1);
					reinterpret_cast<std::string*>(prap->error)->assign("reply=nullptr");
					prap->m_action_cond.notify_one();
					return;
				}
				if (reply->type != REDIS_REPLY_ERROR)
				{
					switch (reply->type)
					{
					case REDIS_REPLY_STRING:// 1
					{
						prap->status = (-1);
						reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_STRING");
					}
					break;
					case REDIS_REPLY_ARRAY:// 2
					{
						prap->status = (-1);
						reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_ARRAY");
					}
					break;
					case  REDIS_REPLY_INTEGER:// 3
					{
						prap->status = (-1);
						reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_INTEGER");
					}
					break;
					case  REDIS_REPLY_NIL:// 4
					{
						prap->status = (-1);
						reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_NIL");
					}
					break;
					case REDIS_REPLY_STATUS:// 5
					{
						prap->status = (-1);
						reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_STATUS");
					}
					break;
					case REDIS_REPLY_DOUBLE:// 7
					{
						prap->status = (-1);
						reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_DOUBLE");
					}
					break;
					case REDIS_REPLY_BOOL:// 8
					{
						prap->status = (0);
						*(reinterpret_cast<bool*>(prap->resp)) = (reply->integer != 0LL);
						reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_BOOL");
					}
					break;
					//case REDIS_REPLY_VERB:// 9
					case REDIS_REPLY_MAP:// 9
					{
						prap->status = (-1);
						reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_MAP/REDIS_REPLY_VERB");
					}
					break;
					case REDIS_REPLY_SET:// 10
					{
						prap->status = (-1);
						reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_SET");
					}
					break;
					case REDIS_REPLY_ATTR:// 11
					{
						prap->status = (-1);
						reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_ATTR");
					}
					break;
					case REDIS_REPLY_PUSH:// 12
					{
						prap->status = (-1);
						reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_PUSH");
					}
					break;
					case REDIS_REPLY_BIGNUM:// 13
					{
						prap->status = (-1);
						reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_BIGNUM");
					}
					break;
					default:
					{
						prap->status = (-1);
						reinterpret_cast<std::string*>(prap->error)->assign("reply->type unknown=" + std::to_string(reply->type));
					}
					break;
					}
					prap->m_action_cond.notify_one();
					return;
				}
				else
				{
					if (reply->str != nullptr)
					{
						reinterpret_cast<std::string*>(prap->error)->assign(reply->str);
						if (((reinterpret_cast<std::string*>(prap->error)->length() > L_MOVED)
							&& (reinterpret_cast<std::string*>(prap->error)->compare(0, L_MOVED, T_MOVED) == 0))
							|| ((reinterpret_cast<std::string*>(prap->error)->length() > L_ASK)
								&& (reinterpret_cast<std::string*>(prap->error)->compare(0, L_ASK, T_ASK) == 0)))
						{
							std::string node_key = reinterpret_cast<std::string*>(prap->error)->substr(reinterpret_cast<std::string*>(prap->error)->rfind(" ") + 1);
							redisAsyncCommand(prab->prauv->get_addr_list()->at(node_key).c, [](redisAsyncContext* c, void* r, void* privdata) {
								struct redis_async_block* prab = reinterpret_cast<struct redis_async_block*>(c->data);
								struct redis_async_param* prap = reinterpret_cast<struct redis_async_param*>(privdata);
								redisReply* reply = reinterpret_cast<redisReply*>(r);
								if (reply == nullptr)
								{
									prap->status = (-1);
									reinterpret_cast<std::string*>(prap->error)->assign("reply=nullptr");
									prap->m_action_cond.notify_one();
									return;
								}
								if (reply->type != REDIS_REPLY_ERROR)
								{
									switch (reply->type)
									{
									case REDIS_REPLY_STRING:// 1
									{
										prap->status = (-1);
										reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_STRING");
									}
									break;
									case REDIS_REPLY_ARRAY:// 2
									{
										prap->status = (-1);
										reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_ARRAY");
									}
									break;
									case  REDIS_REPLY_INTEGER:// 3
									{
										prap->status = (-1);
										reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_INTEGER");
									}
									break;
									case  REDIS_REPLY_NIL:// 4
									{
										prap->status = (-1);
										reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_NIL");
									}
									break;
									case REDIS_REPLY_STATUS:// 5
									{
										prap->status = (-1);
										reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_STATUS");
									}
									break;
									case REDIS_REPLY_DOUBLE:// 7
									{
										prap->status = (-1);
										reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_DOUBLE");
									}
									break;
									case REDIS_REPLY_BOOL:// 8
									{
										prap->status = (0);
										*(reinterpret_cast<bool*>(prap->resp)) = (reply->integer != 0LL);
										reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_BOOL");
									}
									break;
									//case REDIS_REPLY_VERB:// 9
									case REDIS_REPLY_MAP:// 9
									{
										prap->status = (-1);
										reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_MAP/REDIS_REPLY_VERB");
									}
									break;
									case REDIS_REPLY_SET:// 10
									{
										prap->status = (-1);
										reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_SET");
									}
									break;
									case REDIS_REPLY_ATTR:// 11
									{
										prap->status = (-1);
										reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_ATTR");
									}
									break;
									case REDIS_REPLY_PUSH:// 12
									{
										prap->status = (-1);
										reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_PUSH");
									}
									break;
									case REDIS_REPLY_BIGNUM:// 13
									{
										prap->status = (-1);
										reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_BIGNUM");
									}
									break;
									default:
									{
										prap->status = (-1);
										reinterpret_cast<std::string*>(prap->error)->assign("reply->type unknown=" + std::to_string(reply->type));
									}
									break;
									}
									prap->m_action_cond.notify_one();
									return;
								}
								else
								{
									if (reply->str != nullptr)
									{
										prap->status = (-1);
										reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_ERROR, reply->str=" + std::string(reply->str));
										prap->m_action_cond.notify_one();
										return;
									}
									else
									{
										prap->status = (-1);
										reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_ERROR, reply->str=nullptr");
										prap->m_action_cond.notify_one();
										return;
									}
								}
								}, prap, prap->req.c_str());
							uv_async_send(prab->prauv->get_async_ptr());
						}
						else
						{
							prap->status = (-1);
							reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_ERROR, reply->str=" + std::string(reply->str));
							prap->m_action_cond.notify_one();
							return;
						}
					}
					else
					{
						prap->status = (-1);
						reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_ERROR, reply->str=nullptr");
						prap->m_action_cond.notify_one();
						return;
					}
				}

				}, (void*)&rap, rap.req.c_str());
			uv_async_send(this->get_async_ptr());
			std::unique_lock<std::mutex>  ulock(rap.m_action_lock);
			while (rap.status == 1)
			{
				rap.m_action_cond.wait(ulock);
			}
			ulock.unlock();
			locker.unlock();
		}
		else
		{
			rap.status = (-1);
			error.assign("No connected redis instance");
		}
		if (rap.status != 0)
		{
			printf("[%ld]error=%s, result=%lld\n", std::this_thread::get_id(), error.c_str(), resp);
		}
		else
		{
			printf("[%ld]result=%lld\n", std::this_thread::get_id(), resp);
		}
		return rap.status;
	}
	////////////////////////////////////////////////////
	// execute command return array
	// return: 0-success, !0-failure
	int redis_async_execute(std::unordered_map<std::string, std::string>& resp, std::string& error, const std::string& cmd)
	{
		struct redis_async_param rap;
		redis_async_block* async_block_ptr = nullptr;
		for (auto& it : addr_map)
		{
			if (it.second.c != nullptr)
			{
				async_block_ptr = &it.second;
				break;
			}
		}
		if (async_block_ptr != nullptr)
		{
			locker.lock();
			rap.status = 1;
			rap.req = cmd;
			rap.resp = reinterpret_cast<void*>(&resp);
			rap.error = reinterpret_cast<void*>(&error);

			redisAsyncCommand(async_block_ptr->c, [](redisAsyncContext* c, void* r, void* privdata) {
				struct redis_async_block* prab = reinterpret_cast<struct redis_async_block*>(c->data);
				struct redis_async_param* prap = reinterpret_cast<struct redis_async_param*>(privdata);
				redisReply* reply = reinterpret_cast<redisReply*>(r);
				if (reply == nullptr)
				{
					prap->status = (-1);
					reinterpret_cast<std::string*>(prap->error)->assign("reply=nullptr");
					prap->m_action_cond.notify_one();
					return;
				}
				if (reply->type != REDIS_REPLY_ERROR)
				{
					switch (reply->type)
					{
					case REDIS_REPLY_STRING:// 1
					{
						prap->status = (-1);
						reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_STRING");
					}
					break;
					case REDIS_REPLY_ARRAY:// 2
					{
						prap->status = (0);
						reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_ARRAY");
						for (size_t i = 0; i < reply->elements; i += 2)
						{
							reinterpret_cast<std::unordered_map<std::string, std::string>*>(prap->resp)->insert(
								std::unordered_map<std::string, std::string>::value_type(
									std::string(reply->element[i]->str, reply->element[i]->len),
									std::string(reply->element[i + 1]->str, reply->element[i + 1]->len)
								)
							);
						}
					}
					break;
					case  REDIS_REPLY_INTEGER:// 3
					{
						prap->status = (-1);
						reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_INTEGER");
					}
					break;
					case  REDIS_REPLY_NIL:// 4
					{
						prap->status = (-1);
						reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_NIL");
					}
					break;
					case REDIS_REPLY_STATUS:// 5
					{
						prap->status = (-1);
						reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_STATUS");
					}
					break;
					case REDIS_REPLY_DOUBLE:// 7
					{
						prap->status = (-1);
						reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_DOUBLE");
					}
					break;
					case REDIS_REPLY_BOOL:// 8
					{
						prap->status = (-1);
						reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_BOOL");
					}
					break;
					//case REDIS_REPLY_VERB:// 9
					case REDIS_REPLY_MAP:// 9
					{
						prap->status = (-1);
						reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_MAP/REDIS_REPLY_VERB");
					}
					break;
					case REDIS_REPLY_SET:// 10
					{
						prap->status = (-1);
						reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_SET");
					}
					break;
					case REDIS_REPLY_ATTR:// 11
					{
						prap->status = (-1);
						reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_ATTR");
					}
					break;
					case REDIS_REPLY_PUSH:// 12
					{
						prap->status = (-1);
						reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_PUSH");
					}
					break;
					case REDIS_REPLY_BIGNUM:// 13
					{
						prap->status = (-1);
						reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_BIGNUM");
					}
					break;
					default:
					{
						prap->status = (-1);
						reinterpret_cast<std::string*>(prap->error)->assign("reply->type unknown=" + std::to_string(reply->type));
					}
					break;
					}
					prap->m_action_cond.notify_one();
					return;
				}
				else
				{
					if (reply->str != nullptr)
					{
						reinterpret_cast<std::string*>(prap->error)->assign(reply->str);
						if (((reinterpret_cast<std::string*>(prap->error)->length() > L_MOVED)
							&& (reinterpret_cast<std::string*>(prap->error)->compare(0, L_MOVED, T_MOVED) == 0))
							|| ((reinterpret_cast<std::string*>(prap->error)->length() > L_ASK)
								&& (reinterpret_cast<std::string*>(prap->error)->compare(0, L_ASK, T_ASK) == 0)))
						{
							std::string node_key = reinterpret_cast<std::string*>(prap->error)->substr(reinterpret_cast<std::string*>(prap->error)->rfind(" ") + 1);
							redisAsyncCommand(prab->prauv->get_addr_list()->at(node_key).c, [](redisAsyncContext* c, void* r, void* privdata) {
								struct redis_async_block* prab = reinterpret_cast<struct redis_async_block*>(c->data);
								struct redis_async_param* prap = reinterpret_cast<struct redis_async_param*>(privdata);
								redisReply* reply = reinterpret_cast<redisReply*>(r);
								if (reply == nullptr)
								{
									prap->status = (-1);
									reinterpret_cast<std::string*>(prap->error)->assign("reply=nullptr");
									prap->m_action_cond.notify_one();
									return;
								}
								if (reply->type != REDIS_REPLY_ERROR)
								{
									switch (reply->type)
									{
									case REDIS_REPLY_STRING:// 1
									{
										prap->status = (-1);
										reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_STRING");
									}
									break;
									case REDIS_REPLY_ARRAY:// 2
									{
										prap->status = (0);
										reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_ARRAY");
										for (size_t i = 0; i < reply->elements; i += 2)
										{
											reinterpret_cast<std::unordered_map<std::string, std::string>*>(prap->resp)->insert(
												std::unordered_map<std::string, std::string>::value_type(
													std::string(reply->element[i]->str, reply->element[i]->len),
													std::string(reply->element[i + 1]->str, reply->element[i + 1]->len)
												)
											);
										}
									}
									break;
									case  REDIS_REPLY_INTEGER:// 3
									{
										prap->status = (-1);
										reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_INTEGER");
									}
									break;
									case  REDIS_REPLY_NIL:// 4
									{
										prap->status = (-1);
										reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_NIL");
									}
									break;
									case REDIS_REPLY_STATUS:// 5
									{
										prap->status = (-1);
										reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_STATUS");
									}
									break;
									case REDIS_REPLY_DOUBLE:// 7
									{
										prap->status = (-1);
										reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_DOUBLE");
									}
									break;
									case REDIS_REPLY_BOOL:// 8
									{
										prap->status = (-1);
										reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_BOOL");
									}
									break;
									//case REDIS_REPLY_VERB:// 9
									case REDIS_REPLY_MAP:// 9
									{
										prap->status = (-1);
										reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_MAP/REDIS_REPLY_VERB");
									}
									break;
									case REDIS_REPLY_SET:// 10
									{
										prap->status = (-1);
										reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_SET");
									}
									break;
									case REDIS_REPLY_ATTR:// 11
									{
										prap->status = (-1);
										reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_ATTR");
									}
									break;
									case REDIS_REPLY_PUSH:// 12
									{
										prap->status = (-1);
										reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_PUSH");
									}
									break;
									case REDIS_REPLY_BIGNUM:// 13
									{
										prap->status = (-1);
										reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_BIGNUM");
									}
									break;
									default:
									{
										prap->status = (-1);
										reinterpret_cast<std::string*>(prap->error)->assign("reply->type unknown=" + std::to_string(reply->type));
									}
									break;
									}
									prap->m_action_cond.notify_one();
									return;
								}
								else
								{
									if (reply->str != nullptr)
									{
										prap->status = (-1);
										reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_ERROR, reply->str=" + std::string(reply->str));
										prap->m_action_cond.notify_one();
										return;
									}
									else
									{
										prap->status = (-1);
										reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_ERROR, reply->str=nullptr");
										prap->m_action_cond.notify_one();
										return;
									}
								}
								}, prap, prap->req.c_str());
							uv_async_send(prab->prauv->get_async_ptr());
						}
						else
						{
							prap->status = (-1);
							reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_ERROR, reply->str=" + std::string(reply->str));
							prap->m_action_cond.notify_one();
							return;
						}
					}
					else
					{
						prap->status = (-1);
						reinterpret_cast<std::string*>(prap->error)->assign("reply->type=REDIS_REPLY_ERROR, reply->str=nullptr");
						prap->m_action_cond.notify_one();
						return;
					}
				}

				}, (void*)&rap, rap.req.c_str());
			uv_async_send(this->get_async_ptr());
			std::unique_lock<std::mutex>  ulock(rap.m_action_lock);
			while (rap.status == 1)
			{
				rap.m_action_cond.wait(ulock);
			}
			ulock.unlock();
			locker.unlock();
		}
		else
		{
			rap.status = (-1);
			error.assign("No connected redis instance");
		}
		if (rap.status != 0)
		{
			printf("[%ld]error=%s\n", std::this_thread::get_id(), error.c_str());
		}
		else
		{
			printf("[%ld]\n", std::this_thread::get_id());
		}
		for (auto& it : resp)
		{
			printf("%s=%s\n", it.first.c_str(), it.second.c_str());
		}
		return rap.status;
	}

public:
	////////////////////////////////////////////////////
	// Singleton mode
	static CRedisAsyncLibuv* get_instance()
	{
		static CRedisAsyncLibuv instance;
		return &instance;
	}
};