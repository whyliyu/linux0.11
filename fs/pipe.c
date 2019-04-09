/*
 *  linux/fs/pipe.c
 *
 *  (C) 1991  Linus Torvalds
 */

#include <signal.h>

#include <linux/sched.h>
#include <linux/mm.h>	/* for get_free_page */
#include <asm/segment.h>

//从一个管道中读count字节数据到buf
//这个函数只会被sys_read调用
int read_pipe(struct m_inode * inode, char * buf, int count)
{
	int chars, size, read = 0;

	while (count>0) {
		//如果没有数据，唤醒等待在该inode上的进程（写进程），自己sleep等待写进程
		while (!(size=PIPE_SIZE(*inode))) {
			wake_up(&inode->i_wait);
			//如果管道的写进程不关闭了管道句柄，直接返回已读字节数
			if (inode->i_count != 2) /* are there any writers? */
				return read;
			sleep_on(&inode->i_wait);
		}
		chars = PAGE_SIZE-PIPE_TAIL(*inode);
		if (chars > count)
			chars = count;
		if (chars > size)
			chars = size;
		count -= chars; //剩余字节数
		read += chars; //已读字节数
		size = PIPE_TAIL(*inode);
		PIPE_TAIL(*inode) += chars;
		PIPE_TAIL(*inode) &= (PAGE_SIZE-1);
		//将管道中的数据复制到用户空间
		while (chars-->0)
			put_fs_byte(((char *)inode->i_size)[size++],buf++);
	}
	wake_up(&inode->i_wait);
	return read;
}

int write_pipe(struct m_inode * inode, char * buf, int count)
{
	int chars, size, written = 0;

	while (count>0) {
		while (!(size=(PAGE_SIZE-1)-PIPE_SIZE(*inode))) {
			wake_up(&inode->i_wait);
			if (inode->i_count != 2) { /* no readers */
				current->signal |= (1<<(SIGPIPE-1));
				return written?written:-1;
			}
			sleep_on(&inode->i_wait);
		}
		chars = PAGE_SIZE-PIPE_HEAD(*inode);
		if (chars > count)
			chars = count;
		if (chars > size)
			chars = size;
		count -= chars;
		written += chars;
		size = PIPE_HEAD(*inode);
		PIPE_HEAD(*inode) += chars;
		PIPE_HEAD(*inode) &= (PAGE_SIZE-1);
		while (chars-->0)
			((char *)inode->i_size)[size++]=get_fs_byte(buf++);
	}
	wake_up(&inode->i_wait);
	return written;
}

//创建管道的系统调用
//打开一个管道，文件描述符通过参数中的指针返回
//成功返回0，失败返回-1
//正确的使用方式是一个父进程（一般是shell）创建两个子进程，并且让一个子进程使用读句柄，另一个使用写句柄
int sys_pipe(unsigned long * fildes)
{
	struct m_inode * inode;
	struct file * f[2];//两个句柄（指向同一个i节点），一个给写进程，一个给读进程
	int fd[2];
	int i,j;

	j=0;
	for(i=0;j<2 && i<NR_FILE;i++) //找到两个空闲的文件句柄
		if (!file_table[i].f_count)
			(f[j++]=i+file_table)->f_count++;
	if (j==1)
		f[0]->f_count=0;
	if (j<2)
		return -1; //全局文件句柄剩下不够两个，失败，返回 -1
	j=0;
	for(i=0;j<2 && i<NR_OPEN;i++) //将两个空闲的文件句柄挂接到当前进程的task_struct中
		if (!current->filp[i]) {
			current->filp[ fd[j]=i ] = f[j];
			j++;
		}
	if (j==1)
		current->filp[fd[0]]=NULL;
	if (j<2) { //进程文件句柄剩下不够两个，失败，返回 -1
		f[0]->f_count=f[1]->f_count=0;
		return -1;
	}
	if (!(inode=get_pipe_inode())) { //得到一个空闲的inode作为管道inode
		current->filp[fd[0]] =
			current->filp[fd[1]] = NULL;
		f[0]->f_count = f[1]->f_count = 0;
		return -1;
	}
	f[0]->f_inode = f[1]->f_inode = inode;
	f[0]->f_pos = f[1]->f_pos = 0; //读写进程偏移都是0
	f[0]->f_mode = 1;		/* read */ //读进程的模式为读
	f[1]->f_mode = 2;		/* write */ //写进程的模式为写
	put_fs_long(fd[0],0+fildes);
	put_fs_long(fd[1],1+fildes);
	return 0;
}
