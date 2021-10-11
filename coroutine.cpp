#include <iostream>
#include <pthread.h>
#include <list>
#include <string.h>
#include <algorithm>
#include <signal.h>
#include <ucontext.h>
#include <errno.h>
#include <assert.h>
#include <unistd.h>
#include "coroutine.h"

namespace DC{

enum CO_STATUS
{
	COROUTINE_DEAD=0,
	COROUTINE_INIT,
	COROUTINE_SUSPEND,
	COROUTINE_PENDING,
	COROUTINE_RUNNING,
};

void mutex_spin_lock(int * spin_lock)
{
	register int tmp;
	__asm__ __volatile__ (
		"1: 		\n"
		"	cmp	$1, %0 	\n"
		"	je	2f	\n"
		"	pause		\n"
		"	jmp	1b	\n"
		"2:		\n"
		"	xor	%1, %1	\n"
		"	xchg	%0, %1	\n"
		"	test	%1, %1	\n"
		"	je	1b	\n"
		: "=m"(*spin_lock), "=r"(tmp)
	);
}

void mutex_spinun_lock(int *spin_lock)
{
	__asm__ __volatile__ (
		"	movl $1, %0	\n"
		: "=m"(*spin_lock)
	);
}

struct spin_auto_lock
{
	spinlock_t* _spin_lock;
	explicit spin_auto_lock(spinlock_t& spin_lock):_spin_lock(&spin_lock){
		mutex_spin_lock(_spin_lock);
	}
	inline ~spin_auto_lock() {
		if(_spin_lock)
			mutex_spinun_lock(_spin_lock);
	}
};

class mutex
{
	friend class condition;
	pthread_mutex_t _mx;
	mutex(const mutex& rhs){}
public:
	bool isrecursive;
	~mutex(){
		while (pthread_mutex_destroy(&_mx)==EBUSY){ 
			lock(); 
			unlock();
		}
	}
	explicit mutex(bool recursive=false){
		pthread_mutexattr_t attr;
		pthread_mutexattr_init(&attr);
		pthread_mutexattr_settype(&attr,recursive?PTHREAD_MUTEX_RECURSIVE:PTHREAD_MUTEX_NORMAL);
		pthread_mutex_init(&_mx,&attr);
		pthread_mutexattr_destroy(&attr);
	}
	void lock() { pthread_mutex_lock(&_mx);}
	void unlock(){ pthread_mutex_unlock(&_mx);}
	class scoped
	{
		mutex*_mx;
	public:
		~scoped (){_mx->unlock();}
		explicit scoped(mutex&m):_mx(&m){_mx->lock();}
	};
};

class condition
{
	pthread_cond_t _condi;
	condition(const condition& rhs){}
	public:
	~condition (){
		while(pthread_cond_destroy(&_condi) == EBUSY) {
			pthread_cond_broadcast(&_condi);
		}
	}	
	explicit condition() { pthread_cond_init(&_condi,NULL);}
	int wait( mutex &mx) { return pthread_cond_wait( &_condi,&mx._mx);}
	int notify() { return pthread_cond_signal( &_condi);}
};

static pthread_key_t s_key_threads;
static bool s_loop_threads = false;
const int STACK_SIZE = 1024 *1024;
const int DEFAULT_COROUTINE = 16;

struct sandbox;
struct coroutine
{
	coroutine(coroutine_func f,void* u):_func(f),_status(COROUTINE_INIT),_userdata(u),_cap(0),_size(0),_env(NULL),_owner(NULL),_stack(NULL){}
	~coroutine(){
		if(_stack)
			free(_stack);
		_stack = NULL;
		if(_owner)
		_owner->coroutine_release();
	}
	coroutine_func _func;
	int _status;
	void* _userdata;
	ucontext_t _ctx;
	ptrdiff_t _cap;
	ptrdiff_t _size;
	sandbox* _env;
	COROUTINE_HANDLE _owner;
	char*_stack;

	void save_stack(char* top){
		register char *esp asm("esp");
		assert(top -esp <= STACK_SIZE);
		if(_cap<top-esp){
			if(_stack)
				free(_stack);
			_cap =top-esp;
			_stack=(char*)malloc(_cap);
		}
		_size= top -esp;
		memcpy(_stack,esp,_size);
	}
	void attach(COROUTINE_HANDLE o){_owner=o;}
	void handle_release(){
		_owner= NULL;
		asm volatile("": : : "memory");
		_status=COROUTINE_DEAD;
	}
};

void _delete_co(coroutine* p){delete p;}


#ifdef CFIFO
#define MAX_THREAD_RING_MASK (MAX_THREAD_COR_RUNNING-1)
static const size_t MAX_THREAD_DUMP_COUNT=50;
typedef struct coroutine_fifo{
	coroutine* buffer[MAX_THREAD_COR_RUNNING];
	size_t in;
	size_t out;
	coroutine_fifo():in(0),out(0){
		memset(buffer,0,sizeof(buffer));
	}

	bool empty(){return in ==out;}
	void put(coroutine* cor){
		assert((MAX_THREAD_COR_RUNNING - in +out)>1);
		buffer[in&MAX_THREAD_RING_MASK]=cor;
		asm volatile("" : : :"memory");
		++in;
	}

	void put(coroutine** cor,size_t len){
		size_t l;
		len = std::min(len,MAX_THREAD_COR_RUNNING-in+out);
		asm volatile("" : : : "memory");
		l=std::min(len,MAX_THREAD_COR_RUNNING-(in&MAX_THREAD_RING_MASK));
		memcpy(buffer+(in&MAX_THREAD_RING_MASK),cor,l*sizeof(coroutine*));
		memcpy(buffer,buffer+l,(len-l)*sizeof(coroutine*));
		asm volatile("" : : : "memory");
		in +=len;
	}

	coroutine* get(){
		if(out ==in)
			return NULL;
		asm volatile ("": : : "memory");
		return buffer[out++&MAX_THREAD_RING_MASK];
	}

	void dump(coroutine_fifo& rhs){
		size_t len =std::min(in - out,MAX_THREAD_DUMP_COUNT);
		size_t l=std::min(len,MAX_THREAD_COR_RUNNING-(out&MAX_THREAD_RING_MASK));
		asm volatile ("" : : : "memory");
		rhs.put(&buffer[out&MAX_THREAD_RING_MASK],l);
		rhs.put(&buffer[0],len-l);
		asm volatile ("" : : : "memory");
		out +=len;
	}
	void clear(){
		while(out < in)
			delete buffer[out++];
	}
	
} LIST;
#else
typedef std::list<coroutine*> LIST;
#endif

typedef std::list<coroutine*> LIST2;

class schedule;
struct sandbox{

	spinlock_t _lock;
	mutex _mutex;
	condition _condi;
	char _stack[STACK_SIZE];
	ucontext_t _main;// run coroutine context
	coroutine* _running;
	LIST _ready_list;
	LIST _pend_list;
	LIST _dead_list;
	void load(schedule*);

	coroutine* add_pend(coroutine* cor)
	{
#ifdef CFIFO
		_ready_list.put(cor);
#else
		spin_auto_lock guard(_lock);
		_pend_list.push_back(cor);
#endif
		_condi.notify();
		return cor;
	}

	coroutine* pop_ready()
	{
#ifdef CFIFO
		return _ready_list.get();
#else
		if(_ready_list.empty()&&!_pend_list.empty()){
			spin_auto_lock guard(_lock);
			_ready_list.swap(_pend_list);
		}
		coroutine* cor =NULL;
		if(!_ready_list.empty()){
		cor =_ready_list.front();
		_ready_list.pop_front();
		}
		return cor;
#endif
	}

	void add_dead(coroutine* cor){
#ifdef CFIFO
		_dead_list.put(cor);
#else
		_dead_list.push_back(cor);
#endif
	}

	void resume(coroutine* cor){
		assert(!_running);
		_running=cor;
		switch(cor->_status)
		{	
			case COROUTINE_INIT:
			{
				getcontext(&cor->_ctx);
				cor->_env =this;
				cor->_ctx.uc_stack.ss_sp=_stack;
				cor->_ctx.uc_stack.ss_size =sizeof(_stack);
				cor->_ctx.uc_stack.ss_flags=0;
				cor->_ctx.uc_link =&_main;
				makecontext(&(cor->_ctx),(void(*)(void))cor->_func,1,cor->_userdata);
			}
			break;
			case COROUTINE_PENDING:
			{
				memcpy(_stack+STACK_SIZE-cor->_size,cor->_stack,cor->_size);
			}
			break;
			case COROUTINE_DEAD:
			{
				add_dead(cor);
				return;
			}
			break;
			default:
				assert(0);
			break;
		}
		cor->_status =COROUTINE_RUNNING;
		swapcontext(&_main,&cor->_ctx);
		if(_running==cor || cor->_status==COROUTINE_DEAD){//yield should change
			add_dead(cor);
			_running = NULL;
		}
	}
	void yield();
	void reclaim(){
#ifndef CFIFO
	std::for_each(_dead_list.begin(),_dead_list.end(),_delete_co);
#endif
	_dead_list.clear();
	}
};


void _notify_subscriber(condition* cond){
	cond->notify();
}

class schedule {
	typedef std::list<condition*> SUBSCRIBER_LIST;
	schedule():_lock_x(0),_lock_r(0){}
	spinlock_t _lock_x;
	spinlock_t _lock_r;
	SUBSCRIBER_LIST _subscriber_list;
	LIST2 _yield_list;
	LIST _ready_list;
public:
	void subscribe(condition&cond){
		_subscriber_list.push_back(&cond);
	}

	void broadcast(){
		std::for_each(_subscriber_list.begin(),_subscriber_list.end(),_notify_subscriber);
	}

	coroutine* add_ready(coroutine*cor){
		spin_auto_lock guard(_lock_r);
#ifdef CFIFO
		_ready_list.put(cor);
#else
		_ready_list.push_back(cor);
#endif
		broadcast();
		return cor;
	}

	void unload(LIST& list){
		if(_ready_list.empty())
			return;
		spin_auto_lock guard(_lock_r);
#ifdef CFIFO
	_ready_list.dump(list);
#else
	_ready_list.swap(list);
#endif
	}
	void yield(coroutine* cor){
		spin_auto_lock guard(_lock_x);
		cor->_status=COROUTINE_SUSPEND;
		_yield_list.push_back(cor);
	}

	void resume(coroutine*cor){
		if(cor->_status==COROUTINE_SUSPEND){
#ifdef CFIFO
			spin_auto_lock guard2(_lock_r);
#endif
			spin_auto_lock guard(_lock_x);
			LIST2::iterator iter =std::find(_yield_list.begin(),_yield_list.end(),cor);
			assert(iter !=_yield_list.end());
			cor->_status=COROUTINE_PENDING;
			asm volatile("" : : : "memory");
			cor->_env->add_pend(cor);
			_yield_list.erase(iter);
		}
	}

	void clear(){
		spin_auto_lock guard(_lock_x);
#ifndef CFIFO
		std::for_each(_ready_list.begin(),_ready_list.end(),_delete_co);
#endif
		_ready_list.clear();
		std::for_each(_yield_list.begin(),_yield_list.end(),_delete_co);
		_yield_list.clear();
	}

public:
	static schedule* get_instance(){
		static schedule ins;
	return &ins;
	}
};

void sandbox::load(schedule* sche){
	sche->unload(_ready_list);
}

static void destroy_context(void *p){ delete(sandbox*)p;}
void init_threadkey()
{
	pthread_key_create(&s_key_threads,&destroy_context);
}

sandbox* get_sandbox_ins(){
	sandbox* spec = (sandbox*) pthread_getspecific(s_key_threads);
	if(!spec) 
		pthread_setspecific(s_key_threads,spec = new sandbox());
	return spec;
}

void sandbox::yield(){

	assert(_running);
	coroutine* cor =_running;
	_running =NULL;
	if(cor->_status ==COROUTINE_RUNNING){//may COROUTINE_DEAD
		cor->save_stack(_stack+STACK_SIZE);
		schedule::get_instance()->yield(cor);
	}
	swapcontext(&cor->_ctx,&_main);
}

static void *run_coroutine(void *param){

	sandbox* env =get_sandbox_ins();
	schedule::get_instance()->subscribe(env->_condi);
	pthread_detach( pthread_self());

	sigset_t sigs;
	sigemptyset(&sigs);
	sigaddset(&sigs, SIGUSR1);
	sigaddset(&sigs,SIGUSR2);
	sigaddset(&sigs,SIGHUP);
	pthread_sigmask(SIG_BLOCK,&sigs,NULL);

	while(s_loop_threads){
		int cnt=0;
		try {
			while(s_loop_threads){
				coroutine*cor =env->pop_ready();
				if(cor){
					cnt=0;
					env->resume(cor);
				}
				else if(cnt>3){
					cnt=0;
					env->_mutex.lock();
					env->_condi.wait(env->_mutex);
					env->_mutex.unlock();
				}
				else{//for brocast wait event timing sequence bug
					env->reclaim();
					env->load(schedule::get_instance());
					++cnt;
					usleep(4<<cnt);
				}
			}
		}
		catch(...){continue;}
	}
}

void init_coroutine(int thread_count) {
	init_threadkey();
	s_loop_threads = true;
	while(--thread_count>=0){
		pthread_t th;
		pthread_create(&th,NULL,&run_coroutine,NULL);
	}
}

void uninit_coroutine(){
	s_loop_threads =false;
	schedule::get_instance()->clear();
}

coroutine_handle& coroutine_handle::operator =(const coroutine_handle& rhs){
	spin_auto_lock guard(_lock);
	if(&rhs!=this){
		_cid=rhs.shift(this);
	}
	return *this;
}

void coroutine_handle::coroutine_release(){
	spin_auto_lock guard(_lock);
	_cid=0;
}

coroutine_handle::~coroutine_handle(){
	spin_auto_lock guard(_lock);
	if(_cid){
		coroutine* cor=(coroutine*)_cid;
		cor->handle_release();
		_cid=0;
	}
}

COROUTINE_ID coroutine_handle::shift(coroutine_handle* rhs) const
{
	spin_auto_lock guard(_lock);
	COROUTINE_ID tmp =_cid;
	if(_cid){
	coroutine* cor =(coroutine*)_cid;
	cor->attach(rhs);
	_cid=0;
	}
	return tmp;
}

bool coroutine_handle::resume(){
	spin_auto_lock guard(_lock);
	if(!_cid)
		return false;
	schedule::get_instance()->resume((coroutine*)_cid);
	return true;
}

coroutine_able::coroutine_able(){
	_handle=coroutine_new(main,this);
}

coroutine_able::~coroutine_able(){
	if(_handle)
		delete _handle;
}

bool coroutine_able::resume(){
	if(_handle)
		return _handle->resume();
	return false;
}

void coroutine_able::yield(){
	coroutine_yield();
}

void coroutine_able::main (void* ud){
	coroutine_able* cb=(coroutine_able*)ud;
	cb->run();
}

COROUTINE_HANDLE coroutine_new(coroutine_func func,void* ud){
	coroutine* co=new coroutine(func,ud);
	COROUTINE_HANDLE handle=new coroutine_handle((COROUTINE_ID)schedule::get_instance()->add_ready(co));
	co->attach(handle);
	return handle;
}

void coroutine_yield(){
	get_sandbox_ins()->yield();
}

};
