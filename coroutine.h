#ifndef _DC_COROUTINE_H 
#define _DC_COROUTINE_H
namespace DC{

 #define MAX_THREAD_COR_RUNNING 8192

 void init_coroutine(int thread_count =6); 
 void uninit_coroutine();

 // raw callback func invoke if

 typedef void (*coroutine_func)(void* ud); 
 typedef uintptr_t COROUTINE_ID;

 #ifndef spinlock_t 
 #define spinlock_t int 
 #endif

 typedef struct coroutine_handle{

 	mutable spinlock_t _lock; 
 	mutable COROUTINE_ID _cid;

 	coroutine_handle& operator =(const coroutine_handle&); 
	COROUTINE_ID shift(coroutine_handle*) const; 
	void coroutine_release();
	bool resume();
 	explicit coroutine_handle(): _lock(0), _cid(0) {}
	coroutine_handle(COROUTINE_ID c): _lock(0),_cid(c) {}
 	coroutine_handle( const coroutine_handle& rhs): _lock(0){ _cid=rhs.shift(this);} 
	~coroutine_handle(); 
 }*COROUTINE_HANDLE;

 COROUTINE_HANDLE coroutine_new( coroutine_func f, void* ud);
 void coroutine_yield();

 // inhenrit obj if

 class coroutine_able{

 	COROUTINE_HANDLE handle;
private:
	static void main (void* ud);
public:
 	coroutine_able();
 	virtual ~coroutine_able(); 
 	bool resume(); 
	void yield();
	virtual void run() {}
 };

};

#endif

