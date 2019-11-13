// libuv_test.cpp : This file contains the 'main' function. Program execution begins and ends there.
//

#include <iostream>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#ifndef _MSC_VER
#include <unistd.h>
#endif

#include "async_redis_cluster.h"

int test_main() {
	CRedisAsyncLibuv async_redis;
	async_redis.init_addr_list({
		#ifdef _MSC_VER
		struct
		#endif
		redis_async_block("10.0.1.20", 7000),
		#ifdef _MSC_VER
		struct
		#endif
		redis_async_block("10.0.1.20", 7001),
		#ifdef _MSC_VER
		struct
		#endif
		redis_async_block("10.0.1.22", 7005),
		}, "phxUXkvaA4YfS8Cm", true);
	/*async_redis.init_addr_list({
		#ifdef _MSC_VER
		struct
		#endif
		redis_async_block("192.168.100.5", 7000),
		#ifdef _MSC_VER
		struct
		#endif
		 redis_async_block("192.168.100.5", 7001),
		#ifdef _MSC_VER
		struct
		#endif
		 redis_async_block("192.168.100.5", 7002),
		#ifdef _MSC_VER
		struct
		#endif
		 redis_async_block("192.168.100.5", 7003),
		#ifdef _MSC_VER
		struct
		#endif
		 redis_async_block("192.168.100.5", 7004),
		#ifdef _MSC_VER
		struct
		#endif
		 redis_async_block("192.168.100.5", 7005),
		}, "", true);*/
	async_redis.start_redis_loop_thread();
	for (size_t i = 0; i < 1; i++)
	{
		printf("the %dth s\n", i + 1);
		std::this_thread::sleep_for(std::chrono::milliseconds(1000));
	}
	/*std::string error;
	std::string resp;
	redis_async_block* async_block_ptr = nullptr;
	for (auto& it : *g_async_redis->get_addr_list())
	{
		if (it.second.c != nullptr)
		{
			async_block_ptr = &it.second;
			break;
		}
	}
	if (async_block_ptr != nullptr)
	{
		uv_async_send(&async);
		redisAsyncCommand((redisAsyncContext*)(async_block_ptr->c), [](redisAsyncContext* c, void* r, void* privdata) {
			redisReply* reply = reinterpret_cast<redisReply*>(r);
			printf("666GET reply=%x\n", reply);
			if (reply == nullptr)
			{
				return;
			}
			printf("666GET result: %d, %s\n", reply->type, reply->str);
			}, nullptr, "get foo");
	}
	//async_redis.redis_async_execute(resp, error, "get foo");
	//while (1)
	{
		//std::this_thread::sleep_for(std::chrono::milliseconds(3000));
	}
	getchar();*/
	std::list<std::shared_ptr<std::thread>> t_list;
	int n_running = 1;
	for (size_t i = 0; i < 4; i++)
	{
		t_list.push_back(std::make_shared<std::thread>([&n_running](void* p) {
			CRedisAsyncLibuv* async_redis_ptr = reinterpret_cast<CRedisAsyncLibuv*>(p);
			for (size_t i = 0; i < 1000000 && n_running == 1; i++)
			{
				std::string resp;
				std::string error;
				std::string cmd = "get foo";
				if (i % 2 == 0)
				{
					cmd = "set foo " + std::to_string(time(0));

					std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
					async_redis_ptr->redis_async_execute(error, cmd.c_str());
					std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now();
					std::chrono::steady_clock::duration d = end - start;
					if (d == std::chrono::steady_clock::duration::zero())    //0时间长度的表示
					{
						printf("[%d] 0ns\n", i);
					}
					else
					{
						printf("[%d] %lld ns\n", i, std::chrono::duration_cast<std::chrono::microseconds>(d).count());
					}
				}
				else
				{
					std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
					async_redis_ptr->redis_async_execute(resp, error, cmd.c_str());
					std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now();
					std::chrono::steady_clock::duration d = end - start;
					if (d == std::chrono::steady_clock::duration::zero())    //0时间长度的表示
					{
						printf("[%d] 0ns\n", i);
					}
					else
					{
						printf("[%d] %lld ns\n", i, std::chrono::duration_cast<std::chrono::microseconds>(d).count());
					}
				}
			}
			printf("[%ld] exit\n", std::this_thread::get_id());
			}, &async_redis));
	}

	getchar();
	n_running = 0;
	for (auto& it : t_list)
	{
		if (it->joinable())
		{
			it->join();
		}
	}
	async_redis.stop_redis_loop_thread();
	return 0;
}


int main(int argc, char ** argv)
{
    std::cout << "Hello World!\n";
	test_main();
	return 0;
}

// Run program: Ctrl + F5 or Debug > Start Without Debugging menu
// Debug program: F5 or Debug > Start Debugging menu

// Tips for Getting Started: 
//   1. Use the Solution Explorer window to add/manage files
//   2. Use the Team Explorer window to connect to source control
//   3. Use the Output window to see build output and other messages
//   4. Use the Error List window to view errors
//   5. Go to Project > Add New Item to create new code files, or Project > Add Existing Item to add existing code files to the project
//   6. In the future, to open this project again, go to File > Open > Project and select the .sln file
