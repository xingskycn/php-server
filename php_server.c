/*
  +----------------------------------------------------------------------+
  | PHP Version 7                                                        |
  +----------------------------------------------------------------------+
  | Copyright (c) 1997-2014 The PHP Group                                |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.01 of the PHP license,      |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.php.net/license/3_01.txt                                  |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
  | Author:                                                              |
  +----------------------------------------------------------------------+
*/

/* $Id$ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_ini.h"
#include "php_globals.h"
#include "php_main.h"
#include "ext/standard/info.h"
#include "php_php_server.h"

/* 包含需要用到的头文件 */
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <malloc.h>

ZEND_DECLARE_MODULE_GLOBALS(php_server)
//struct zend_php_server_globals php_server_globals;

/* True global resources - no need for thread safety here */
static int le_php_server;

/* {{{ PHP_INI
 */
PHP_INI_BEGIN()
	STD_PHP_INI_ENTRY("php_server.process_number", "7", PHP_INI_ALL, OnUpdateLong, process_number, zend_php_server_globals, php_server_globals)
	STD_PHP_INI_ENTRY("php_server.master_name", "php server master", PHP_INI_ALL, OnUpdateString, master_name, zend_php_server_globals, php_server_globals)
	STD_PHP_INI_ENTRY("php_server.worker_name", "php server worker", PHP_INI_ALL, OnUpdateString, worker_name, zend_php_server_globals, php_server_globals)
PHP_INI_END()
/* }}} */



/* 定义一些常亮 */
//最大的子进程数量
#define MAX_PROCESS_NUMBER 16

//每个子进程最多能够处理的客户连接数量
#define MAX_USER_CONNECT 65536

//epoll最多能够的处理的事件的数量
#define MAX_EPOLL_NUMBER 10000


//父子管道通讯的数据
#define PHP_SERVER_DATA_INIT '0'
#define PHP_SERVER_DATA_CONN '1'
#define PHP_SERVER_DATA_KILL '9'

//调试的宏
#define PHP_SERVER_DEBUG printf
//长度要加上最后的\0结束符
//#define PHP_SERVER_RESPONSE "HTTP/1.1 200 OK\r\nServer: php_server 1.0\r\nContent-Length: 11\r\n\r\n0123456789"
//#define PHP_SERVER_HTTP_SEND(sockfd) send(sockfd,PHP_SERVER_RESPONSE,sizeof(PHP_SERVER_RESPONSE),0); \
									 php_server_epoll_del_fd(process_global->epoll_fd,sockfd); \
									 printf("manual close client %d\n",sockfd);
#define PHP_SERVER_HTTP_SEND(sockfd) //

#define BUFFER_SIZE 8096
char recv_buffer[BUFFER_SIZE];


/* 监听的socket_fd  */
int socket_fd_global;

extern char ** environ;

server_process *  process_global;

static zend_class_entry * php_server_class_entry;

static HashTable callback_ht;

/*
	argsinfo
*/
ZEND_BEGIN_ARG_INFO_EX(arginfo_php_server_create, 0, 0, 2)
	ZEND_ARG_INFO(0,ip)
	ZEND_ARG_INFO(0,port)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_php_server_bind, 0, 0, 2)
	ZEND_ARG_INFO(0,event)
	ZEND_ARG_INFO(0,callback)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_php_server_send, 0, 0, 2)
	ZEND_ARG_INFO(0,sockfd)
	ZEND_ARG_INFO(0,message)
	ZEND_ARG_INFO(0,flush)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_php_server_set, 0, 0, 2)
	ZEND_ARG_INFO(0,key)
	ZEND_ARG_INFO(0,value)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_php_server_get, 0, 0, 0)
	ZEND_ARG_INFO(0,key)
ZEND_END_ARG_INFO()


ZEND_BEGIN_ARG_INFO_EX(arginfo_php_server_close, 0, 0, 0)
	ZEND_ARG_INFO(0,sockfd)
ZEND_END_ARG_INFO()

const zend_function_entry php_server_class_functions[] = {
		ZEND_FENTRY(__construct,PHP_FN(php_server_create),arginfo_php_server_create,ZEND_ACC_PUBLIC | ZEND_ACC_CTOR)
		ZEND_FENTRY(bind,PHP_FN(php_server_bind),arginfo_php_server_bind,ZEND_ACC_PUBLIC)
		//ZEND_FENTRY(send,PHP_FN(php_server_send),arginfo_php_server_send,ZEND_ACC_PUBLIC)
		ZEND_FENTRY(set,PHP_FN(php_server_set),arginfo_php_server_set,ZEND_ACC_PUBLIC)
		ZEND_FENTRY(get,PHP_FN(php_server_get),arginfo_php_server_get,ZEND_ACC_PUBLIC)
		ZEND_FENTRY(run,PHP_FN(php_server_run),NULL,ZEND_ACC_PUBLIC)
		PHP_FE_END
};

/* {{{ 测试模块是否正常加载
 */
/*PHP_FUNCTION(test_php_server){
	int ret,process_number=PHP_SERVER_G(process_number);

	PHP_SERVER_DEBUG("function is ok !\n");

	ret = php_server_setup_socket("127.0.0.1",9000);
	PHP_SERVER_DEBUG("setup_socket:%d\n",ret);

	ret = php_server_setup_process_pool(socket_fd_global,process_number);
	PHP_SERVER_DEBUG("setup_process_pool:%d\n",ret);

	PHP_SERVER_DEBUG("server %d is running.\n",process_global->process_index);
	php_server_run();
	PHP_SERVER_DEBUG("server %d is stopping.\n",process_global->process_index);

	ret = php_server_shutdown_process_pool(process_number);
	PHP_SERVER_DEBUG("shutdown_process_pool:%d pid:%d\n",ret,getpid());

	ret = php_server_shutdown_socket();
	PHP_SERVER_DEBUG("shutdown_socket:%d pid:%d\n------------------------\n",ret,getpid());		

	RETURN_STRING("php-server");
}*/
/* }}} */
PHP_FUNCTION(php_server_create)
{
	char * ip;
	size_t ip_len,port;
	if(zend_parse_parameters(ZEND_NUM_ARGS(),"sl",&ip,&ip_len,&port) == FAILURE){
		return;
	}
	zval * this_settings = zend_read_property(php_server_class_entry,getThis(),"_settings",sizeof("_settings")-1,0,NULL);
	array_init(this_settings);
	add_assoc_stringl_ex(this_settings,"ip",sizeof("ip")-1,ip,ip_len);
	add_assoc_long_ex(this_settings,"port",sizeof("port")-1,(zend_long)port);
	zend_update_property(php_server_class_entry,getThis(),"_settings",sizeof("_settings")-1,this_settings);
}
PHP_FUNCTION(php_server_bind)
{
	zval * event , *callback;
	if(zend_parse_parameters(ZEND_NUM_ARGS(),"zz",&event,&callback) == FAILURE || Z_TYPE_P(event) != IS_STRING){
		return;
	}
	zend_hash_add(&callback_ht,Z_STR_P(event),callback);
	zval_ptr_dtor(event);
	zval_ptr_dtor(callback);
}

PHP_FUNCTION(php_server_send)
{
	char * message;
	size_t sockfd,message_len;
	zend_bool is_flush = 0;
	int ret = -1;
	FILE * fp;
	if(zend_parse_parameters(ZEND_NUM_ARGS(),"ls|b",&sockfd,&message,&message_len,&is_flush) != FAILURE){
		ret = send((int)sockfd,message,message_len,0);
		if(is_flush){
			if(NULL != (fp = fdopen(sockfd,"rw"))){
				fflush(fp);
				//fclose(fp);
				fp = NULL;
				PHP_SERVER_DEBUG("flush %d size\n",ret);
			}
		}
	}
	RETURN_LONG(ret);
}



PHP_FUNCTION(php_server_set)
{
	char * key_str, * tmp_str;
	size_t key_len,tmp_len;
	zval *value;
	if(zend_parse_parameters(ZEND_NUM_ARGS(),"sz",&key_str,&key_len,&value) == FAILURE){
		return;
	}else{
		zval * this_settings = zend_read_property(php_server_class_entry,getThis(),"_settings",sizeof("_settings")-1,0,NULL);
		zend_hash_str_update(Z_ARRVAL_P(this_settings),key_str,key_len,value);
	}
}
PHP_FUNCTION(php_server_get)
{
	char * key = NULL;
	size_t key_len;
	zval * this_settings = zend_read_property(php_server_class_entry,getThis(),"_settings",sizeof("_settings")-1,0,NULL);
	if(1!= ZEND_NUM_ARGS() || zend_parse_parameters(ZEND_NUM_ARGS(),"s",&key,&key_len) == FAILURE){
		RETURN_ZVAL(this_settings,1,NULL);
	}else{
		zval * retval = zend_hash_str_find(Z_ARRVAL_P(this_settings),key,key_len);
		if(retval){
			RETURN_ZVAL(retval,1,NULL);
		}
	}
}
PHP_FUNCTION(php_server_run){
	zval * this_settings = zend_read_property(php_server_class_entry,getThis(),"_settings",sizeof("_settings")-1,0,NULL);
	zval * ip = zend_hash_str_find(Z_ARRVAL_P(this_settings),"ip",sizeof("ip")-1);
	zval * port = zend_hash_str_find(Z_ARRVAL_P(this_settings),"port",sizeof("port")-1);
	if(Z_TYPE_P(ip)!= IS_STRING  || Z_TYPE_P(port)!=IS_LONG){
		php_error_docref(NULL,E_ERROR,"ip must be string and port must be int");
		return;
	}

	int ret,process_number=PHP_SERVER_G(process_number);

	//PHP_SERVER_DEBUG("function is ok !\n");

	ret = php_server_setup_socket(Z_STRVAL_P(ip),(int)Z_LVAL_P(port));
	//ret = php_server_setup_socket("127.0.0.1",9009);
	PHP_SERVER_DEBUG("setup_socket:%d\n",ret);
	if(ret){
		php_error_docref(NULL,E_ERROR,"create socket error\n");
		return;
	}

	ret = php_server_setup_process_pool(socket_fd_global,process_number);
	//PHP_SERVER_DEBUG("setup_process_pool:%d\n",ret);

	PHP_SERVER_DEBUG("server %d is running.\n",process_global->process_index);
	php_server_run();
	PHP_SERVER_DEBUG("server %d is stopping.\n",process_global->process_index);

	ret = php_server_shutdown_process_pool(process_number);
	//PHP_SERVER_DEBUG("shutdown_process_pool:%d pid:%d\n",ret,getpid());

	ret = php_server_shutdown_socket();
	//PHP_SERVER_DEBUG("shutdown_socket:%d pid:%d\n------------------------\n",ret,getpid());

}


PHP_FUNCTION(php_server_close)
{
	int sockfd;
	if(zend_parse_parameters(ZEND_NUM_ARGS(),"l",&sockfd) == FAILURE){
		RETURN_FALSE;
	}
	
	php_server_epoll_del_fd(process_global->epoll_fd,sockfd);
	RETURN_TRUE;
}
/* {{{ php_php_server_init_globals
 */
static void php_php_server_init_globals(zend_php_server_globals *php_server_globals)
{
    php_server_globals->process_number = 0;
	php_server_globals->master_name = NULL;
	php_server_globals->worker_name = NULL;
}
/* }}} */

/* {{{ PHP_MINIT_FUNCTION
 */
PHP_MINIT_FUNCTION(php_server)
{
	REGISTER_INI_ENTRIES();

	zend_class_entry ce;
	INIT_CLASS_ENTRY(ce,"php_server",php_server_class_functions);
	php_server_class_entry = zend_register_internal_class(&ce);
	/* MUST CHANGE TO ZEND_ACC_PRIVATE */
	zend_declare_property_null(php_server_class_entry,"_settings",sizeof("_settings")-1,ZEND_ACC_PUBLIC);

	/* CALLBACK HASHTABLE INIT */
	zend_hash_init(&callback_ht, 0, NULL, NULL, 1);
	return SUCCESS;
}
/* }}} */

/* {{{ PHP_MSHUTDOWN_FUNCTION
 */
PHP_MSHUTDOWN_FUNCTION(php_server)
{
	UNREGISTER_INI_ENTRIES();
	/* CALLBACK HASHTABLE CLEAN */
	zend_hash_destroy(&callback_ht);
	return SUCCESS;
}
/* }}} */

/* Remove if there's nothing to do at request start */
/* {{{ PHP_RINIT_FUNCTION
 */
PHP_RINIT_FUNCTION(php_server)
{
#if defined(COMPILE_DL_PHP_SERVER) && defined(ZTS)
	ZEND_TSRMLS_CACHE_UPDATE;
#endif
	return SUCCESS;
}
/* }}} */

/* Remove if there's nothing to do at request end */
/* {{{ PHP_RSHUTDOWN_FUNCTION
 */
PHP_RSHUTDOWN_FUNCTION(php_server)
{
	return SUCCESS;
}
/* }}} */

/* {{{ PHP_MINFO_FUNCTION
 */
PHP_MINFO_FUNCTION(php_server)
{
	php_info_print_table_start();
	php_info_print_table_header(2, "php_server support", "enabled");
	php_info_print_table_end();

	DISPLAY_INI_ENTRIES();
}
/* }}} */

/* 一些常用的函数  */

/* 修改进程的显示名称 */

void php_server_set_proc_name(int argc,char ** argv,char * name){

	size_t size = 0;
	char * last,*p;
	int i,len;
	/* 加上最后的字符串结束符 */
	for(i=0;environ[i];i++){
		size += strlen(environ[i]) + 1;
	}
	char * newEnviron = (char * ) malloc(size);
	for(i=0;environ[i];i++){
		len = strlen(environ[i]) + 1;
		memcpy(newEnviron,environ[i],len);
		environ[i] =  newEnviron;
		newEnviron += len;
	}
	last = argv[0];
	for(i=0;i<argc;i++){
		last += strlen(argv[i]) + 1;
	}
	last += size;
	argv[1] = 0;
	p = argv[0];
	len = last - p;
	memset(p,0,len);
	strncpy(p,name,len);
}

/*直接设置php的标题的名称*/

void php_set_proc_name(char * name){
	zval argv[1];
	zval retval;
	zval function_name;
	ZVAL_STRING(&function_name,"cli_set_process_title");
	ZVAL_STRING(&argv[0],name);
	if(call_user_function_ex(EG(function_table),NULL,&function_name,&retval,1,argv,1,NULL) == FAILURE){
		php_error_docref(NULL, E_WARNING, "Could not call the cli_set_process_name");
	}
	zval_ptr_dtor(&argv[0]);
	zval_ptr_dtor(&function_name);
}

/* 设置文件描述符为非阻塞  */
int php_server_set_nonblock(int fd){
	int old_option,new_option;
	old_option = fcntl(fd,F_GETFL);
	new_option = old_option | O_NONBLOCK;
	fcntl(fd,F_SETFL,new_option);
	return old_option;
}

/* 为某个fd向epoll事件表添加边沿触发式可读事件  */
void php_server_epoll_add_read_fd(int epoll_fd,int fd){
	struct epoll_event event;
	event.data.fd = fd;
	event.events = EPOLLIN | EPOLLET;
	epoll_ctl(epoll_fd,EPOLL_CTL_ADD,fd, &event);
	php_server_set_nonblock(fd);
}

/* 把某个fd移除出epoll事件表*/
void php_server_epoll_del_fd(int epoll_fd,int fd){
	epoll_ctl(epoll_fd, EPOLL_CTL_DEL,fd,0);
	close(fd);
}


zend_bool php_server_setup_socket(char * ip,int port){
	
	int ret;
	struct sockaddr_in address;
	
	socket_fd_global = socket(PF_INET,SOCK_STREAM,0);
	if(socket_fd_global<0){
		PHP_SERVER_DEBUG("socket error\n");
		return FAILURE;
	}

	bzero(&address,sizeof(address));

	address.sin_family = AF_INET;

	inet_pton(AF_INET,ip,&address.sin_addr);

	address.sin_port = htons(port);

	ret = bind(socket_fd_global,(struct sockaddr * )&address,sizeof(address));

	if(ret == -1){
		PHP_SERVER_DEBUG("bind error\n");
		return FAILURE;
	}
	
	ret = listen(socket_fd_global,5);

	if(ret == -1){
		PHP_SERVER_DEBUG("listen error\n");
		return FAILURE;
	}

	return SUCCESS;
}

/* 关闭监听的socket */

zend_bool php_server_shutdown_socket(){
	close(socket_fd_global);
	return SUCCESS;
}

/*创建进程池*/

zend_bool php_server_setup_process_pool(int socket_fd,	unsigned int process_number){
	
	int i,ret;
	pid_t pid;

	process_global = (server_process * ) malloc(sizeof(server_process));

	process_global->pipe_fd = (int **)malloc(sizeof(int * ) * process_number);

	for(i=0; i<process_number;i++){
		process_global->pipe_fd[i] = (int *) malloc(sizeof(int) * 2);		
	}	

	process_global->socket_fd = socket_fd;

	process_global->process_number = 0;
	
	process_global->is_stop = 1;	

	for(i=0;	i<process_number;	i++){

		process_global->socket_fd = socket_fd_global;
		
		ret = socketpair(PF_UNIX,	SOCK_STREAM,	0,process_global->pipe_fd[i]);

		if(ret!=0){
			return FAILURE;
		}

		pid = fork();

		if(pid > 0){
			PHP_SERVER_DEBUG("child %d is created\n",pid);
			if(0 == i){
				//php_set_proc_name("php server master");
				php_set_proc_name(PHP_SERVER_G(master_name));
				process_global->child_pid = (pid_t * ) malloc(sizeof(pid_t) * process_number);
				process_global->process_number = process_number;
			}
			process_global->process_index = -1;
			process_global->child_pid[i] = pid;
			close(process_global->pipe_fd[i][1]);
			continue;
		}else if(0 == pid){
			//php_set_proc_name("php server worker");
			php_set_proc_name(PHP_SERVER_G(worker_name));
			process_global->process_index = i;
			close(process_global->pipe_fd[i][0]);
			break;
		}else{
			return FAILURE;
		}
	}

	return SUCCESS;
}
/* 信号处理函数  */
void php_server_sig_handler(int signal_no){
	
	static int killed_child = 0;
	int i;
	pid_t pid;	
	char data = PHP_SERVER_DATA_KILL;

	PHP_SERVER_DEBUG("server %d get %d(%d:%d:%d)\n",process_global->process_index,signal_no,SIGCHLD,SIGTERM,SIGINT);

	if(process_global->process_index == -1){
		switch(signal_no){
			/* 如果子进程退出，那么设置其pid为-1并关闭其管道  */
			case SIGCHLD:
			for(;;){
				if((pid = waitpid(-1,NULL,WNOHANG)) > 0){
					for(i=0; i<process_global->process_number; i++){
						PHP_SERVER_DEBUG("%d ",process_global->child_pid[i]);
						if(pid == process_global->child_pid[i]){
							process_global->child_pid[i] = -1;
							close(process_global->pipe_fd[i][0]);
							PHP_SERVER_DEBUG("child %d:%d out\n",pid,i);
							killed_child++;
							break;
						}
					}
				}else{
					PHP_SERVER_DEBUG("sigchld break\n");
					break;
				}
				/* 当退出的进程数量达到子进程的总量，那么父进程也紧跟着退出 */
				if(killed_child == process_global->process_number){
					process_global->is_stop = 1;
				}
				PHP_SERVER_DEBUG("pid %d get,killed child %d\n",pid,killed_child);
			}
			break;

			case SIGTERM:
			case SIGINT:
			/* 向所有还存活的进程发送信号 */
			for(i=0;i<process_global->process_number;i++){
				if(-1 != process_global->child_pid[i]){
					//kill(process_global->child_pid[i],SIGTERM);
					send(process_global->pipe_fd[i][0],(char *) &data,sizeof(data),0);
				}
			}
			break;
		}
	}else{
		switch(signal_no){
			case SIGTERM:
			case SIGINT:
			process_global->is_stop = 1;
			break;
		}
	}
}

/* 启动初始化函数 */
zend_bool php_server_run_init(){
	process_global->epoll_fd = epoll_create(5);
	if(process_global->epoll_fd == -1){
		return FAILURE;
	}
	process_global->is_stop = 0;
	/*设置信号处理函数*/
	signal(SIGCHLD,php_server_sig_handler);
	signal(SIGTERM,php_server_sig_handler);
	signal(SIGINT,php_server_sig_handler);
	return SUCCESS;
}

/* 启动函数的清理函数 */
zend_bool php_server_clear_init(){

	close(process_global->epoll_fd);
	return SUCCESS;
}

/* 启动父进程*/
int php_server_run_master_process(){
		
	struct epoll_event events[MAX_EPOLL_NUMBER];
	int i,number,sock_fd,child_index = 0,child_pid,child_pipe,alive_child;
	char data = PHP_SERVER_DATA_CONN;

	php_server_run_init();
	
	/* 父进程监听global_socket */
	php_server_epoll_add_read_fd(process_global->epoll_fd, process_global->socket_fd);

	PHP_SERVER_DEBUG("master while\n");
	
	while(!process_global->is_stop){
		number = epoll_wait(process_global->epoll_fd,events,MAX_EPOLL_NUMBER,-1);
		PHP_SERVER_DEBUG("epoll come in master:%d break:%d\n",number,number<0 && errno!=EINTR);
		if(number<0 && errno != EINTR){
			break; 
		}
		
		for(i=0;i<number;i++){
			sock_fd = events[i].data.fd;
				/*说明有的新的连接到来，那么轮询一个进程处理这个连接*/
			if(sock_fd == process_global->socket_fd){
				alive_child = process_global->process_number;
				while(-1 == process_global->child_pid[child_index]){
					child_index = (child_index+1)%process_global->process_number;
					alive_child--;
					if(0 == alive_child){
						break;
					}
				}
				/* 如果子进程存活量为0了，那么就kill自己,然后父进程会通知所有存在的子进程杀掉自己*/
				if(alive_child == 0){
					PHP_SERVER_DEBUG("child none, master start kill itself\n");
					//kill(getpid(),SIGTERM);
					
					/* 向所有还存活的进程发送信号 */
					int j;
					char data = PHP_SERVER_DATA_KILL;
					for(j=0;j<process_global->process_number;j++){
						if(-1 != process_global->child_pid[j]){
							//kill(process_global->child_pid[i],SIGTERM);
							send(process_global->pipe_fd[j][0],(char *) &data,sizeof(data),0);
						}
					}
					break;
				}
				child_pid = process_global->child_pid[child_index];
				child_pipe = process_global->pipe_fd[child_index][0];
				send(child_pipe,(char *) & data,sizeof(data) , 0);
				child_index = (child_index+1)%process_global->process_number;
			}
		}
	}
	
	php_server_clear_init();
	if(number<0){
		return errno;
	}
	return 0;
}

/* 子进程循环读取消息 */

int php_server_recv_from_client(int sock_fd){
	int ret;

	bzero(recv_buffer,sizeof(recv_buffer));	

	ret = recv(sock_fd,recv_buffer,sizeof(recv_buffer),0);

	/* 说明读操作错误，那么不再监听这个连接 */
	if(ret<0){
		if(errno != EAGAIN){
			php_server_epoll_del_fd(process_global->epoll_fd,sock_fd);	
		}
		PHP_SERVER_DEBUG("worker recv error\n");

	}else if(ret == 0){
		/* 说明客户端关闭了连接 */

		/*callback close*/
		struct sockaddr_in client;
		size_t client_len = sizeof(client);
		getpeername(sock_fd,(struct sockaddr *)&client,(socklen_t *)&client_len);
		zval * close_cb = zend_hash_str_find(&callback_ht,"close",sizeof("close")-1);
		char client_ip_str[INET_ADDRSTRLEN];
		long port = (long)ntohs(client.sin_port);
		if(NULL!=close_cb && port > 0 && NULL != inet_ntop(AF_INET,(void *)&client.sin_addr.s_addr,client_ip_str,INET_ADDRSTRLEN)){
			zval args[3], retval;
			ZVAL_LONG(&args[0],sock_fd);
			ZVAL_STRING(&args[1],client_ip_str);
			ZVAL_LONG(&args[2],port);
			if(call_user_function(EG(function_table),NULL,close_cb,&retval,3,args)){
				php_error_docref(NULL,E_WARNING,"close error.\n");
			}
			PHP_SERVER_DEBUG("worker %d close %d after call\n",process_global->process_index,sock_fd);
			zval_dtor(&args[0]);
			zval_dtor(&args[1]);
			zval_dtor(&args[2]);
		}
		/*callback close end*/

		php_server_epoll_del_fd(process_global->epoll_fd,sock_fd);
		//PHP_SERVER_DEBUG("client %d closed\n",sock_fd);
	}else{
		//PHP_SERVER_DEBUG("%d recv from %d:%s\n",process_global->process_index,sock_fd,recv_buffer);
		/* callback receive */
		struct sockaddr_in client;
		size_t client_len = sizeof(client);
		getpeername(sock_fd,(struct sockaddr *)&client,(socklen_t *)&client_len);
		zval * receive_cb = zend_hash_str_find(&callback_ht,"receive",sizeof("receive")-1);
		char client_ip_str[INET_ADDRSTRLEN];
		long port = (long)ntohs(client.sin_port);
		if(NULL!=receive_cb && port > 0 && NULL != inet_ntop(AF_INET,(void *)&client.sin_addr.s_addr,client_ip_str,INET_ADDRSTRLEN)){
			zval args[4], retval;
			ZVAL_LONG(&args[0],sock_fd);
			ZVAL_STRINGL(&args[1],recv_buffer,ret);
			ZVAL_STRING(&args[2],client_ip_str);
			ZVAL_LONG(&args[3],port);
			if(call_user_function(EG(function_table),NULL,receive_cb,&retval,4,args)){
				php_error_docref(NULL,E_WARNING,"receive error.\n");
			}
			PHP_SERVER_DEBUG("worker %d receive %d after call\n",process_global->process_index,sock_fd);
			zval_dtor(&args[0]);
			zval_dtor(&args[1]);
			zval_dtor(&args[2]);
			zval_dtor(&args[3]);
		}
		/* callback receive end */
	}
	return ret;
}



/* 启动子进程  */
int php_server_run_worker_process(){
	
	struct epoll_event events[MAX_EPOLL_NUMBER];
	int i,number,ret,sock_fd,conn_fd,parent_pipe_fd = process_global->pipe_fd[process_global->process_index][1];
	char data = PHP_SERVER_DATA_INIT;
	struct sockaddr_in client;
	socklen_t client_len = sizeof(client);
	php_server_run_init();
	
	php_server_epoll_add_read_fd(process_global->epoll_fd,parent_pipe_fd);
	/* 添加父管道的可读事件，以便知道有新的连接到来了 */
	while(!process_global->is_stop){
		number = epoll_wait(process_global->epoll_fd,events,MAX_EPOLL_NUMBER,-1);
		PHP_SERVER_DEBUG("epoll come in worker:%d break:%d\n",number,number<0 && errno!=EINTR);
		if(number<0 && errno != EINTR){
			return	errno; 
		}
		
		for(i=0;i<number;i++){
			sock_fd = events[i].data.fd;
			if(sock_fd == parent_pipe_fd && (events[i].events & EPOLLIN)){
				ret = recv(sock_fd,(char *) & data, sizeof(data),0);
				if( ( ret < 0 && errno != EAGAIN) || ret == 0 ){
					continue;
				}else{
					if(data==PHP_SERVER_DATA_CONN){
						//有新的连接到来
						bzero(&client,client_len);
						conn_fd = accept(process_global->socket_fd,(struct sockaddr *) & client, &client_len);
					
						PHP_SERVER_DEBUG("worker %d accepted %d\n",process_global->process_index,conn_fd);
						if(conn_fd < 0){
							continue;
						}
						php_server_epoll_add_read_fd(process_global->epoll_fd,conn_fd);
						/* call accept*/
						zval * accept_cb = zend_hash_str_find(&callback_ht,"accept",sizeof("accept")-1);
						char client_ip_str[INET_ADDRSTRLEN];
						long port = (long)ntohs(client.sin_port);
						if(NULL!=accept_cb && port > 0 && NULL != inet_ntop(AF_INET,(void *)&client.sin_addr.s_addr,client_ip_str,INET_ADDRSTRLEN)){
							zval args[3], retval;
							ZVAL_LONG(&args[0],conn_fd);
							ZVAL_STRING(&args[1],client_ip_str);
							ZVAL_LONG(&args[2],port);
							if(call_user_function(EG(function_table),NULL,accept_cb,&retval,3,args)){
								php_error_docref(NULL,E_WARNING,"accept error.\n");
							}
							PHP_SERVER_DEBUG("worker %d accept %d after call\n",process_global->process_index,conn_fd);
							zval_dtor(&args[0]);
							zval_dtor(&args[1]);
							zval_dtor(&args[2]);
						}
						/*call accept end*/
					}else if(data==PHP_SERVER_DATA_KILL){
						process_global->is_stop = 1;
						break;
					}
				}
			/*这里除开父进程发送的消息，就只有客户端的消息到来了*/
			}else if(events[i].events & EPOLLIN){
				
				php_server_recv_from_client(sock_fd);
			}
		}	
	}

	php_server_clear_init();
	return 0;
}

/* 父子进程统一启动进程的函数*/
void php_server_run(){
	if(process_global->process_index == -1){
		php_server_run_master_process();
	}else{
		php_server_run_worker_process();
	}
}


/* 销毁进程池  */
zend_bool php_server_shutdown_process_pool(unsigned int process_number){
	int i;
	for(i = 0; i<process_number; i++){
		free(process_global->pipe_fd[i]);
	}
	free(process_global->pipe_fd);
	if(process_global->process_index == -1){
		free(process_global->child_pid);
	}
	free(process_global);
	return SUCCESS;
}
/* {{{ php_server_functions[]
 *
 * Every user visible function must have an entry in php_server_functions[].
 */
const zend_function_entry php_server_functions[] = {
//	PHP_FE(test_php_server , NULL)
	PHP_FE(php_server_send,arginfo_php_server_send)
	PHP_FE(php_server_close,arginfo_php_server_close)
	PHP_FE_END	/* Must be the last line in php_server_functions[] */
};
/* }}} */

/* {{{ php_server_module_entry
 */
zend_module_entry php_server_module_entry = {
	STANDARD_MODULE_HEADER,
	"php_server",
	php_server_functions,
	PHP_MINIT(php_server),
	PHP_MSHUTDOWN(php_server),
	PHP_RINIT(php_server),		/* Replace with NULL if there's nothing to do at request start */
	PHP_RSHUTDOWN(php_server),	/* Replace with NULL if there's nothing to do at request end */
	PHP_MINFO(php_server),
	PHP_PHP_SERVER_VERSION,
	STANDARD_MODULE_PROPERTIES
};
/* }}} */

#ifdef COMPILE_DL_PHP_SERVER
#ifdef ZTS
ZEND_TSRMLS_CACHE_DEFINE;
#endif
ZEND_GET_MODULE(php_server)
#endif

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */
