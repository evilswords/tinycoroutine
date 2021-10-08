#include <stdio.h>
#include <iostream>
#include <sys/time.h>
#include <set>
#include "coroutine.h"

#define LOOP_COUNT 1000

void test_cor_func(void *ud){
	int pcount = 0;
	while(++pcount < LOOP_COUNT){
		usleep(30);
		DC::coroutine_yield();
	}
	int *p = (int*)ud;
	__sync_add_and_fetch(p,1);
}

void simple1(int thc,int cxc){
	DC::COROUTINE_HANDLE cids[cxc];
	size_t index = 0;
	int total = 0;
	for(;index < cxc; ++index)
		cids[index] = DC::coroutine_new(test_cor_func,&total);

	while(cxc){
		usleep(1);
		if(index == cxc){
			index=0;
		}
		if(!cids[index]->resume()){
			delete cids[index];
			if(cxc != index+1)
				memmove(&cids[index],&cids[index+1],sizeof(DC::COROUTINE_HANDLE)*(cxc-index));
		}
		else
			++index;		
	}
	std::cout << total << std::endl;
}

class test_cor_obj : public DC::coroutine_able
{
	int *pt;
	public:
	test_cor_obj(int*t):pt(t) {}
	void run()
	{
		int pcount = 0;
		while (++pcount < LOOP_COUNT){
			usleep(30);
			yield();
		}
		__sync_add_and_fetch(pt,1);
	}
};

void simple2(int thc,int cxc){
	test_cor_obj* cids[cxc];
	size_t index = 0;
	int total = 0;
	for(;index < cxc; ++index)
		cids[index] = new test_cor_obj(&total);

	while(cxc){
		usleep(1);
		if(index == cxc){
			index=0;
		}
		if(!cids[index]->resume()){
			delete cids[index];
			if(cxc != index+1)
				memmove(&cids[index],&cids[index+1],sizeof(test_cor_obj*)*(cxc-index));
		}
		else
			++index;		
	}
	std::cout << total << std::endl;
}

int main(int argc, char* argv[]){
	int thc = 6;
	int cxc = 200;
	bool flag = false;
	if(argc > 1) thc = atoi(argv[1]);
	if(argc > 2) cxc = atoi(argv[2]);
	if(argc > 3) flag = true;
	DC::init_coroutine(thc);

	timeval ll1,ll2;
	gettimeofday(&ll1,NULL);

	if(flag)
		simple1(thc,cxc);
	else
		simple2(thc,cxc);

	gettimeofday(&ll2,NULL);
	unsigned long long delta = (ll2.tv_usec-ll1.tv_usec)+(ll2.tv_sec-ll1.tv_sec)*1000000;
	printf("use %8lld us\n",delta);

	DC::uninit_coroutine();
	return 0;
}

