/*
 * UNIX		An implementation of the AF_UNIX network domain for the
 *		LINUX operating system.  UNIX is implemented using the
 *		BSD Socket interface as the means of communication with
 *		the user level.
 *
 * Version:	@(#)sock.c	1.0.5	05/25/93
 *
 * Authors:	Orest Zborowski, <obz@Kodak.COM>
 *		Ross Biro, <bir7@leland.Stanford.Edu>
 *		Fred N. van Kempen, <waltje@uWalt.NL.Mugnet.ORG>
 *
 * Fixes:
 *		Alan Cox	:	Verify Area
 *		NET2E Team	:	Page fault locks
 *	Dmitry Gorodchanin	:	/proc locking
 *
 * To Do:
 *	Some nice person is looking into Unix sockets done properly. NET3
 *	will replace all of this and include datagram sockets and socket
 *	options - so please stop asking me for them 8-)
 *
 *
 *		This program is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or(at your option) any later version.
 */

#include <linux/kernel.h>
#include <linux/major.h>
#include <linux/signal.h>
#include <linux/sched.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/stat.h>
#include <linux/socket.h>
#include <linux/un.h>
#include <linux/fcntl.h>
#include <linux/termios.h>
#include <linux/sockios.h>
#include <linux/net.h>
#include <linux/fs.h>
#include <linux/malloc.h>

#include <asm/system.h>
#include <asm/segment.h>

#include <stdarg.h>

#include "unix.h"

/*
 *	Because these have the address in them they casually waste an extra 8K of kernel data
 *	space that need not be wasted.
 */
 
struct unix_proto_data unix_datas[NSOCKETS_UNIX];

static int unix_proto_create(struct socket *sock, int protocol);
static int unix_proto_dup(struct socket *newsock, struct socket *oldsock);
static int unix_proto_release(struct socket *sock, struct socket *peer);
static int unix_proto_bind(struct socket *sock, struct sockaddr *umyaddr,
			   int sockaddr_len);
static int unix_proto_connect(struct socket *sock, struct sockaddr *uservaddr,
			      int sockaddr_len, int flags);
static int unix_proto_socketpair(struct socket *sock1, struct socket *sock2);
static int unix_proto_accept(struct socket *sock, struct socket *newsock, 
			     int flags);
static int unix_proto_getname(struct socket *sock, struct sockaddr *usockaddr,
			      int *usockaddr_len, int peer);
static int unix_proto_read(struct socket *sock, char *ubuf, int size,
			   int nonblock);
static int unix_proto_write(struct socket *sock, char *ubuf, int size,
			    int nonblock);
static int unix_proto_select(struct socket *sock, int sel_type, select_table * wait);
static int unix_proto_ioctl(struct socket *sock, unsigned int cmd,
			    unsigned long arg);
static int unix_proto_listen(struct socket *sock, int backlog);
static int unix_proto_send(struct socket *sock, void *buff, int len,
			    int nonblock, unsigned flags);
static int unix_proto_recv(struct socket *sock, void *buff, int len,
			    int nonblock, unsigned flags);
static int unix_proto_sendto(struct socket *sock, void *buff, int len,
			      int nonblock, unsigned flags,
			      struct sockaddr *addr, int addr_len);
static int unix_proto_recvfrom(struct socket *sock, void *buff, int len,
				int nonblock, unsigned flags,
				struct sockaddr *addr, int *addr_len);

static int unix_proto_shutdown(struct socket *sock, int how);

static int unix_proto_setsockopt(struct socket *sock, int level, int optname,
				  char *optval, int optlen);
static int unix_proto_getsockopt(struct socket *sock, int level, int optname,
				  char *optval, int *optlen);


static inline int min(int a, int b)
{
	if (a < b)
		return(a);
	return(b);
}



/* Support routines doing anti page fault locking 
 * FvK & Matt Dillon (borrowed From NET2E3)
 */

/*
 * Locking for unix-domain sockets.  We don't use the socket structure's
 * wait queue because it is allowed to 'go away' outside of our control,
 * whereas unix_proto_data structures stick around.
 */
 
static void unix_lock(struct unix_proto_data *upd)
{
	while (upd->lock_flag)
		sleep_on(&upd->wait);
	upd->lock_flag = 1;
}


static void unix_unlock(struct unix_proto_data *upd)
{
	upd->lock_flag = 0;
	wake_up(&upd->wait);
}

/*
 *	We don't have to do anything. 
 */
 
static int unix_proto_listen(struct socket *sock, int backlog)
{
	return(0);
}

/*
 *	Until the new NET3 Unix code is done we have no options.
 */

static int unix_proto_setsockopt(struct socket *sock, int level, int optname,
		      char *optval, int optlen)
{
	return(-EOPNOTSUPP);
}


static int unix_proto_getsockopt(struct socket *sock, int level, int optname,
		      char *optval, int *optlen)
{
	return(-EOPNOTSUPP);
}


/*
 *	SendTo() doesn't matter as we also have no Datagram support!
 */

static int unix_proto_sendto(struct socket *sock, void *buff, int len, int nonblock, 
		  unsigned flags,  struct sockaddr *addr, int addr_len)
{
	return(-EOPNOTSUPP);
}     

static int unix_proto_recvfrom(struct socket *sock, void *buff, int len, int nonblock, 
		    unsigned flags, struct sockaddr *addr, int *addr_len)
{
	return(-EOPNOTSUPP);
}     

/*
 *	You can't shutdown a unix domain socket.
 */

static int unix_proto_shutdown(struct socket *sock, int how)
{
	return(-EOPNOTSUPP);
}


/*
 *	Send data to a unix socket.
 */
 
static int unix_proto_send(struct socket *sock, void *buff, int len, int nonblock,
		unsigned flags)
{
	if (flags != 0) 
		return(-EINVAL);
	return(unix_proto_write(sock, (char *) buff, len, nonblock));
}


/* 
 *	Receive data. This version of AF_UNIX also lacks MSG_PEEK 8(
 */
 
static int unix_proto_recv(struct socket *sock, void *buff, int len, int nonblock,
		unsigned flags)
{
	if (flags != 0) 
		return(-EINVAL);
	return(unix_proto_read(sock, (char *) buff, len, nonblock));
}

/*
 *	Given an address and an inode go find a unix control structure
 */
 
static struct unix_proto_data *
unix_data_lookup(struct sockaddr_un *sockun, int sockaddr_len,
		 struct inode *inode)
{
	 struct unix_proto_data *upd;

	 for(upd = unix_datas; upd <= last_unix_data; ++upd) 
	 {
		if (upd->refcnt > 0 && upd->socket &&
			upd->socket->state == SS_UNCONNECTED &&
			upd->sockaddr_un.sun_family == sockun->sun_family &&
			upd->inode == inode) 
			
			return(upd);
	}
	return(NULL);
}

/*
 *	We allocate a page of data for the socket. This is woefully inadequate and helps cause vast
 *	amounts of excess task switching and blocking when transferring stuff like bitmaps via X.
 *	It doesn't help this problem that the Linux scheduler is desperately in need of a major 
 *	rewrite. Somewhere near 16K would be better maybe 32.
 */
// 分配一个没有被使用的unix_proto_data结构
static struct unix_proto_data *
unix_data_alloc(void)
{
	struct unix_proto_data *upd;

	cli();
	for(upd = unix_datas; upd <= last_unix_data; ++upd) 
	{	// 没有被使用
		if (!upd->refcnt) 
		{	// 初始化数据
			upd->refcnt = -1;	/* unix domain socket not yet initialised - bgm */
			sti();
			upd->socket = NULL;
			upd->sockaddr_len = 0;
			upd->sockaddr_un.sun_family = 0;
			upd->buf = NULL;
			upd->bp_head = upd->bp_tail = 0;
			upd->inode = NULL;
			upd->peerupd = NULL;
			return(upd);
		}
	}
	sti();
	return(NULL);
}

/*
 *	The data area is owned by all its users. Thus we need to track owners
 *	carefully and not free data at the wrong moment. These look like they need
 *	interrupt protection but they don't because no interrupt ever fiddles with
 *	these counts. With an SMP Linux you'll need to protect these!
 */

static inline void unix_data_ref(struct unix_proto_data *upd)
{
	if (!upd) 
	{
		return;
	}
	++upd->refcnt;
}


static void unix_data_deref(struct unix_proto_data *upd)
{
	if (!upd) 
	{
		return;
	}
	//引用数为1说明没人使用了，则释放该结构对应的内存
	if (upd->refcnt == 1) 
	{
		if (upd->buf) 
		{
			free_page((unsigned long)upd->buf);
			upd->buf = NULL;
			upd->bp_head = upd->bp_tail = 0;
		}
	}
	--upd->refcnt;
}


/*
 *	Upon a create, we allocate an empty protocol data,
 *	and grab a page to buffer writes.
 */
 
static int unix_proto_create(struct socket *sock, int protocol)
{
	struct unix_proto_data *upd;

	/*
	 *	No funny SOCK_RAW stuff
	 */
	 
	if (protocol != 0) 
	{
		return(-EINVAL);
	}
	// 分配一个unix_proto_data结构体
	if (!(upd = unix_data_alloc())) 
	{
		printk("UNIX: create: can't allocate buffer\n");
		return(-ENOMEM);
	}
	// 给unix_proto_data的buf字段分配一个页大小的内存
	if (!(upd->buf = (char*) get_free_page(GFP_USER))) 
	{
		printk("UNIX: create: can't get page!\n");
		unix_data_deref(upd);
		return(-ENOMEM);
	}
	upd->protocol = protocol;
	// 关联unix_proto_data对应的socket结构
	upd->socket = sock;
	// 关联socket对应的unix_proto_data结构
	UN_DATA(sock) = upd;
	// 标记unix_proto_data已被使用
	upd->refcnt = 1;	/* Now it's complete - bgm */
	return(0);
}

/*
 *	Duplicate a socket.
 */
/* 
	创建一个新的unix_proto_data结构和socket关联,newsock由上层新创建的，即首先创建了一个新的socket结构，
	根据oldsock的协议类型，创建了一个新的unix_proto_data和newsock关联
*/
static int unix_proto_dup(struct socket *newsock, struct socket *oldsock)
{
	struct unix_proto_data *upd = UN_DATA(oldsock);
	return(unix_proto_create(newsock, upd->protocol));
}


/*
 *	Release a Unix domain socket.
 */
 
static int unix_proto_release(struct socket *sock, struct socket *peer)
{
	struct unix_proto_data *upd = UN_DATA(sock);

	if (!upd) 
		return(0);
	// 重置unix_proto_data结构他数据，引用计数减一，有对端的话把对端的unix_proto_data计数也减一，因为不再引用他
	if (upd->socket != sock) 
	{
		printk("UNIX: release: socket link mismatch!\n");
		return(-EINVAL);
	}

	if (upd->inode) 
	{
		iput(upd->inode);
		upd->inode = NULL;
	}

	UN_DATA(sock) = NULL;
	upd->socket = NULL;

	if (upd->peerupd)
		unix_data_deref(upd->peerupd);
	unix_data_deref(upd);
	return(0);
}


/*
 *	Bind a name to a socket.
 *	This is where much of the work is done: we allocate a fresh page for
 *	the buffer, grab the appropriate inode and set things up.
 *
 *	FIXME: what should we do if an address is already bound?
 *	  Here we return EINVAL, but it may be necessary to re-bind.
 *	  I think thats what BSD does in the case of datagram sockets...
 */
// 把sockaddr的内容存在unix_proto_data中，并创建一个文件
static int unix_proto_bind(struct socket *sock, struct sockaddr *umyaddr,
		int sockaddr_len)
{
	char fname[UNIX_PATH_MAX + 1];
	struct unix_proto_data *upd = UN_DATA(sock);
	unsigned long old_fs;
	int i;

	if (sockaddr_len <= UN_PATH_OFFSET ||
		sockaddr_len > sizeof(struct sockaddr_un)) 
	{
		return(-EINVAL);
	}
	if (upd->sockaddr_len || upd->inode) 
	{
		/*printk("UNIX: bind: already bound!\n");*/
		return(-EINVAL);
	}
	// sockaddr_un兼容sockaddr结构
	memcpy(&upd->sockaddr_un, umyaddr, sockaddr_len);
	/*
		UN_PATH_OFFSET为sun_path在sockaddr_un结构中的偏移，
		sockaddr_len-UN_PATH_OFFSET等于sun_path的最后一个字符+1的位置
	*/
	upd->sockaddr_un.sun_path[sockaddr_len-UN_PATH_OFFSET] = '\0';
	if (upd->sockaddr_un.sun_family != AF_UNIX) 
	{
		return(-EINVAL);
	}
	// 把sun_path的值放到fname中
	memcpy(fname, upd->sockaddr_un.sun_path, sockaddr_len-UN_PATH_OFFSET);
	fname[sockaddr_len-UN_PATH_OFFSET] = '\0';
	old_fs = get_fs();
	set_fs(get_ds());
	// 新建一个inode节点，文件名是fname，标记是一个socket类型
	i = do_mknod(fname, S_IFSOCK | S_IRWXUGO, 0);

	if (i == 0) 
		i = open_namei(fname, 0, S_IFSOCK, &upd->inode, NULL); // &upd->inode保存打开文件对应的inode节点
	set_fs(old_fs);
	if (i < 0) 
	{
/*		printk("UNIX: bind: can't open socket %s\n", fname);*/
		if(i==-EEXIST)
			i=-EADDRINUSE;
		return(i);
	}
	upd->sockaddr_len = sockaddr_len;	/* now it's legal */
	
	return(0);
}


/*
 *	Perform a connection. we can only connect to unix sockets
 *	(I can't for the life of me find an application where that
 *	wouldn't be the case!)
 */
// 根据uservaddr传入的信息从找到一个unix_proto_data结构，即服务器
static int unix_proto_connect(struct socket *sock, struct sockaddr *uservaddr,
		   int sockaddr_len, int flags)
{
	char fname[sizeof(((struct sockaddr_un *)0)->sun_path) + 1];
	struct sockaddr_un sockun;
	struct unix_proto_data *serv_upd;
	struct inode *inode;
	unsigned long old_fs;
	int i;

	if (sockaddr_len <= UN_PATH_OFFSET ||
		sockaddr_len > sizeof(struct sockaddr_un)) 
	{
		return(-EINVAL);
	}

	if (sock->state == SS_CONNECTING) 
		return(-EINPROGRESS);
	if (sock->state == SS_CONNECTED)
		return(-EISCONN);

	memcpy(&sockun, uservaddr, sockaddr_len);
	sockun.sun_path[sockaddr_len-UN_PATH_OFFSET] = '\0';
	if (sockun.sun_family != AF_UNIX) 
	{
		return(-EINVAL);
	}

/*
 * Try to open the name in the filesystem - this is how we
 * identify ourselves and our server. Note that we don't
 * hold onto the inode that long, just enough to find our
 * server. When we're connected, we mooch off the server.
 */

	memcpy(fname, sockun.sun_path, sockaddr_len-UN_PATH_OFFSET);
	fname[sockaddr_len-UN_PATH_OFFSET] = '\0';
	old_fs = get_fs();
	set_fs(get_ds());
	// 根据传入的路径打开该文件，把inode存在inode变量里
	i = open_namei(fname, 2, S_IFSOCK, &inode, NULL);
	set_fs(old_fs);
	if (i < 0) 
	{
		return(i);
	}
	// 从unix_proto_data表中找到对应的unix_proto_data结构  
	serv_upd = unix_data_lookup(&sockun, sockaddr_len, inode);
	iput(inode);
	// 没有则说明服务端不存在
	if (!serv_upd) 
	{
		return(-EINVAL);
	}
	
	if ((i = sock_awaitconn(sock, serv_upd->socket, flags)) < 0) 
	{
		return(i);
	}
	// conn为服务端socket
	if (sock->conn) 
	{	// 服务端unix_proto_data结构引用数加一，并指向服务端unix_proto_data结构
		unix_data_ref(UN_DATA(sock->conn));
		UN_DATA(sock)->peerupd = UN_DATA(sock->conn); /* ref server */
	}
	return(0);
}


/*
 *	To do a socketpair, we just connect the two datas, easy!
 *	Since we always wait on the socket inode, they're no contention
 *	for a wait area, and deadlock prevention in the case of a process
 *	writing to itself is, ignored, in true unix fashion!
 */
// 新建两个socket，互相指向对端的unix_proto_data结构，返回两个文件描述符 
static int unix_proto_socketpair(struct socket *sock1, struct socket *sock2)
{
	struct unix_proto_data *upd1 = UN_DATA(sock1), *upd2 = UN_DATA(sock2);

	unix_data_ref(upd1);
	unix_data_ref(upd2);
	upd1->peerupd = upd2;
	upd2->peerupd = upd1;
	return(0);
}


/* 
 *	On accept, we ref the peer's data for safe writes. 
 */

static int unix_proto_accept(struct socket *sock, struct socket *newsock, int flags)
{
	struct socket *clientsock;

/*
 * If there aren't any sockets awaiting connection,
 * then wait for one, unless nonblocking.
 */
	// 拿到客户端连接队列
	while(!(clientsock = sock->iconn)) 
	{	
		// 为空并且设置了非阻塞直接返回
		if (flags & O_NONBLOCK) 
			return(-EAGAIN);
		// 设置等待连接标记
		sock->flags |= SO_WAITDATA;
		// 阻塞
		interruptible_sleep_on(sock->wait);
		// 清除等待连接标记
		sock->flags &= ~SO_WAITDATA;
		if (current->signal & ~current->blocked) 
		{
			return(-ERESTARTSYS);
		}
	}
/*
 * Great. Finish the connection relative to server and client,
 * wake up the client and return the new fd to the server.
 */
	// 更新服务端的连接队列，摘下了第一个节点
	sock->iconn = clientsock->next;
	clientsock->next = NULL;
	// 新生成的socket结构，对端指向客户端
	newsock->conn = clientsock;
	// 互相引用，设置状态为已连接
	clientsock->conn = newsock;
	clientsock->state = SS_CONNECTED;
	newsock->state = SS_CONNECTED;
	// unix_proto_data结构的引用数加1
	unix_data_ref(UN_DATA(clientsock));
	// 把unix_proto_data的数据复制到sock结构中，保存客户端的路径信息
	UN_DATA(newsock)->peerupd	     = UN_DATA(clientsock);
	UN_DATA(newsock)->sockaddr_un        = UN_DATA(sock)->sockaddr_un;
	UN_DATA(newsock)->sockaddr_len       = UN_DATA(sock)->sockaddr_len;
	// 唤醒被阻塞的客户端队列
	wake_up_interruptible(clientsock->wait);
	sock_wake_async(clientsock, 0);
	return(0);
}


/*
 *	Gets the current name or the name of the connected socket. 
 */
// 获取socket绑定的地址
static int unix_proto_getname(struct socket *sock, struct sockaddr *usockaddr,
		   int *usockaddr_len, int peer)
{
	struct unix_proto_data *upd;
	int len;
	// 取本端还是对端的
	if (peer) 
	{
		if (sock->state != SS_CONNECTED) 
		{
			return(-EINVAL);
		}
		// 获取对端的unix_proto_data结构
		upd = UN_DATA(sock->conn);
	}
	else
		upd = UN_DATA(sock);

	len = upd->sockaddr_len;
	memcpy(usockaddr, &upd->sockaddr_un, len);
 	*usockaddr_len=len;
	return(0);
}


/* 
 *	We read from our own buf. 
 */
 
static int unix_proto_read(struct socket *sock, char *ubuf, int size, int nonblock)
{
	struct unix_proto_data *upd;
	int todo, avail;

	if ((todo = size) <= 0) 
		return(0);

	upd = UN_DATA(sock);
	// 看buf中有多少数据可读
	while(!(avail = UN_BUF_AVAIL(upd))) 
	{
		if (sock->state != SS_CONNECTED) 
		{
			return((sock->state == SS_DISCONNECTING) ? 0 : -EINVAL);
		}
		// 没有数据，但是以非阻塞模式，直接返回
		if (nonblock) 
			return(-EAGAIN);
		// 阻塞等待数据
		sock->flags |= SO_WAITDATA;
		interruptible_sleep_on(sock->wait);
		// 唤醒后清除等待标记位
		sock->flags &= ~SO_WAITDATA;
		if (current->signal & ~current->blocked) 
		{
			return(-ERESTARTSYS);
		}
	}

/*
 *	Copy from the read buffer into the user's buffer,
 *	watching for wraparound. Then we wake up the writer.
 */
    // 加锁
	unix_lock(upd);
	do 
	{
		int part, cando;

		if (avail <= 0) 
		{
			printk("UNIX: read: AVAIL IS NEGATIVE!!!\n");
			send_sig(SIGKILL, current, 1);
			return(-EPIPE);
		}
		// 要读的比可读的多，则要读的为可读的数量
		if ((cando = todo) > avail) 
			cando = avail;
		// 有一部分数据在队尾，一部分在队头，则先读队尾的,bp_tail表示可写空间的最后一个字节加1，即可读的第一个字节
		if (cando >(part = BUF_SIZE - upd->bp_tail)) 
			cando = part;
		memcpy_tofs(ubuf, upd->buf + upd->bp_tail, cando);
		// 更新bp_tail，可写空间增加
		upd->bp_tail =(upd->bp_tail + cando) &(BUF_SIZE-1);
		// 更新用户的buf指针
		ubuf += cando;
		// 还需要读的字节数
		todo -= cando;
		if (sock->state == SS_CONNECTED)
		{
			wake_up_interruptible(sock->conn->wait);
			sock_wake_async(sock->conn, 2);
		}
		avail = UN_BUF_AVAIL(upd);
	} 
	while(todo && avail);// 还有数据并且还没读完则继续
	unix_unlock(upd);
	return(size - todo);// 要读的减去读了的
}


/*
 *	We write to our peer's buf. When we connected we ref'd this
 *	peer so we are safe that the buffer remains, even after the
 *	peer has disconnected, which we check other ways.
 */
 
static int unix_proto_write(struct socket *sock, char *ubuf, int size, int nonblock)
{
	struct unix_proto_data *pupd;
	int todo, space;

	if ((todo = size) <= 0)
		return(0);
	if (sock->state != SS_CONNECTED) 
	{
		if (sock->state == SS_DISCONNECTING) 
		{
			send_sig(SIGPIPE, current, 1);
			return(-EPIPE);
		}
		return(-EINVAL);
	}
	// 获取对端的unix_proto_data字段
	pupd = UN_DATA(sock)->peerupd;	/* safer than sock->conn */
	// 还有多少空间可写
	while(!(space = UN_BUF_SPACE(pupd))) 
	{
		sock->flags |= SO_NOSPACE;
		if (nonblock) 
			return(-EAGAIN);
		sock->flags &= ~SO_NOSPACE;
		interruptible_sleep_on(sock->wait);
		if (current->signal & ~current->blocked) 
		{
			return(-ERESTARTSYS);
		}
		if (sock->state == SS_DISCONNECTING) 
		{
			send_sig(SIGPIPE, current, 1);
			return(-EPIPE);
		}
	}

/*
 *	Copy from the user's buffer to the write buffer,
 *	watching for wraparound. Then we wake up the reader.
 */
   
	unix_lock(pupd);

	do 
	{
		int part, cando;

		if (space <= 0) 
		{
			printk("UNIX: write: SPACE IS NEGATIVE!!!\n");
			send_sig(SIGKILL, current, 1);
			return(-EPIPE);
		}

		/*
		 *	We may become disconnected inside this loop, so watch
		 *	for it (peerupd is safe until we close).
		 */
		 
		if (sock->state == SS_DISCONNECTING) 
		{
			send_sig(SIGPIPE, current, 1);
			unix_unlock(pupd);
			return(-EPIPE);
		}
		// 需要写的比能写的多
		if ((cando = todo) > space) 
			cando = space;
		// 可写空间一部分在队头一部分在队尾，则先写队尾的，再写队头的
		if (cando >(part = BUF_SIZE - pupd->bp_head))
			cando = part;
	
		memcpy_fromfs(pupd->buf + pupd->bp_head, ubuf, cando);
		// 更新可写地址，可写空间减少，处理回环情况
		pupd->bp_head =(pupd->bp_head + cando) &(BUF_SIZE-1);
		// 更新用户的buf指针
		ubuf += cando;
		// 还需要写多少个字
		todo -= cando;
		if (sock->state == SS_CONNECTED)
		{
			wake_up_interruptible(sock->conn->wait);
			sock_wake_async(sock->conn, 1);
		}
		space = UN_BUF_SPACE(pupd);
	}
	while(todo && space);

	unix_unlock(pupd);
	return(size - todo);
}

/*
 *	Select on a unix domain socket.
 */

static int unix_proto_select(struct socket *sock, int sel_type, select_table * wait)
{
	struct unix_proto_data *upd, *peerupd;

	/* 
	 *	Handle server sockets specially.
	 */
	if (sock->flags & SO_ACCEPTCON) 
	{
		if (sel_type == SEL_IN) 
		{
			if (sock->iconn) 
				return(1);
			select_wait(sock->wait, wait);
			return(sock->iconn ? 1 : 0);
		}
		select_wait(sock->wait, wait);
		return(0);
	}

	if (sel_type == SEL_IN) 
	{
		upd = UN_DATA(sock);
		if (UN_BUF_AVAIL(upd))	/* even if disconnected */
			return(1);
		else if (sock->state != SS_CONNECTED) 
		{
			return(1);
		}
		select_wait(sock->wait,wait);
		return(0);
	}

	if (sel_type == SEL_OUT) 
	{
		if (sock->state != SS_CONNECTED) 
		{
			return(1);
		}
		peerupd = UN_DATA(sock->conn);
		if (UN_BUF_SPACE(peerupd) > 0) 
			return(1);
		select_wait(sock->wait,wait);
		return(0);
	}

	/*
	 * Exceptions - SEL_EX 
	 */

	return(0);
}


/*
 *	ioctl() calls sent to an AF_UNIX socket
 */

static int unix_proto_ioctl(struct socket *sock, unsigned int cmd, unsigned long arg)
{
	struct unix_proto_data *upd, *peerupd;
	int er;

	upd = UN_DATA(sock);
	peerupd = (sock->state == SS_CONNECTED) ? UN_DATA(sock->conn) : NULL;

	switch(cmd) 
	{
		case TIOCINQ:
			if (sock->flags & SO_ACCEPTCON) 
				return(-EINVAL);
			er=verify_area(VERIFY_WRITE,(void *)arg, sizeof(unsigned long));
			if(er)
				return er;
			if (UN_BUF_AVAIL(upd) || peerupd)
				put_fs_long(UN_BUF_AVAIL(upd),(unsigned long *)arg);
			else
				put_fs_long(0,(unsigned long *)arg);
			break;
		case TIOCOUTQ:
			if (sock->flags & SO_ACCEPTCON) 
				return(-EINVAL);
			er=verify_area(VERIFY_WRITE,(void *)arg, sizeof(unsigned long));
			if(er)
				return er;
			if (peerupd) 
				put_fs_long(UN_BUF_SPACE(peerupd),(unsigned long *)arg);
			else
				put_fs_long(0,(unsigned long *)arg);
			break;
		default:
			return(-EINVAL);
	}
	return(0);
}


static struct proto_ops unix_proto_ops = {
	AF_UNIX,
	unix_proto_create,
	unix_proto_dup,
	unix_proto_release,
	unix_proto_bind,
	unix_proto_connect,
	unix_proto_socketpair,
	unix_proto_accept,
	unix_proto_getname,
	unix_proto_read,
	unix_proto_write,
	unix_proto_select,
	unix_proto_ioctl,
	unix_proto_listen,
	unix_proto_send,
	unix_proto_recv,
	unix_proto_sendto,
	unix_proto_recvfrom,
	unix_proto_shutdown,
	unix_proto_setsockopt,
	unix_proto_getsockopt,
	NULL				/* unix_proto_fcntl	*/
};

/*
 *	Initialise the Unix domain protocol.
 */

void unix_proto_init(struct net_proto *pro)
{
	struct unix_proto_data *upd;

	/*
	 *	Tell SOCKET that we are alive... 
	 */

	(void) sock_register(unix_proto_ops.family, &unix_proto_ops);

	for(upd = unix_datas; upd <= last_unix_data; ++upd) 
	{
		upd->refcnt = 0;
	}
}
