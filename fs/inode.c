/*
 *  linux/fs/inode.c
 *
 *  (C) 1991  Linus Torvalds
 */

#include <string.h>
#include <sys/stat.h>

#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <asm/system.h>

struct m_inode inode_table[NR_INODE]={{0,},};

static void read_inode(struct m_inode * inode);
static void write_inode(struct m_inode * inode);

static inline void wait_on_inode(struct m_inode * inode)
{
	cli();
	while (inode->i_lock)
		sleep_on(&inode->i_wait);
	sti();
}

static inline void lock_inode(struct m_inode * inode)
{
	cli();
	while (inode->i_lock)
		sleep_on(&inode->i_wait);
	inode->i_lock=1;
	sti();
}

static inline void unlock_inode(struct m_inode * inode)
{
	inode->i_lock=0;
	wake_up(&inode->i_wait);
}

void invalidate_inodes(int dev)
{
	int i;
	struct m_inode * inode;

	inode = 0+inode_table;
	for(i=0 ; i<NR_INODE ; i++,inode++) {
		wait_on_inode(inode);
		if (inode->i_dev == dev) {
			if (inode->i_count)
				printk("inode in use on removed disk\n\r");
			inode->i_dev = inode->i_dirt = 0;
		}
	}
}

//遍历inode_table，将所有的脏inode刷写到设备（其实是刷写到缓冲区）
void sync_inodes(void)
{
	int i;
	struct m_inode * inode;

	inode = 0+inode_table;
	for(i=0 ; i<NR_INODE ; i++,inode++) {
		wait_on_inode(inode);
		if (inode->i_dirt && !inode->i_pipe)
			write_inode(inode);
	}
}

//得到inode指向的文件的第block个文件块在设备上的位置，create表示如果这个文件块不存在则创建它
static int _bmap(struct m_inode * inode,int block,int create)
{
	struct buffer_head * bh;
	int i;

	if (block<0)
		panic("_bmap: block<0");
	if (block >= 7+512+512*512)
		panic("_bmap: block>big");
	if (block<7) { //如果是直接数据块
		if (create && !inode->i_zone[block])
			//如果直接数据块不存在，且create为1，那么在设备上创建一个直接数据块（通过数据块位图）
			//并且更新inode的数据块引用
			if (inode->i_zone[block]=new_block(inode->i_dev)) {
				inode->i_ctime=CURRENT_TIME;
				inode->i_dirt=1;
			}
		return inode->i_zone[block];
	}
	block -= 7;
	if (block<512) { //如果是一级索引的数据块
		if (create && !inode->i_zone[7])
			if (inode->i_zone[7]=new_block(inode->i_dev)) {
				inode->i_dirt=1;
				inode->i_ctime=CURRENT_TIME;
			}
		if (!inode->i_zone[7])
			return 0;
		if (!(bh = bread(inode->i_dev,inode->i_zone[7])))
			return 0;
		i = ((unsigned short *) (bh->b_data))[block];
		if (create && !i)
			if (i=new_block(inode->i_dev)) {
				((unsigned short *) (bh->b_data))[block]=i;
				bh->b_dirt=1;
			}
		brelse(bh);
		return i;
	}
	//到这里，说明请求的数据块是二级索引数据块
	block -= 512;
	if (create && !inode->i_zone[8])
		if (inode->i_zone[8]=new_block(inode->i_dev)) {
			inode->i_dirt=1;
			inode->i_ctime=CURRENT_TIME;
		}
	if (!inode->i_zone[8])
		return 0;
	if (!(bh=bread(inode->i_dev,inode->i_zone[8])))
		return 0;
	i = ((unsigned short *)bh->b_data)[block>>9];
	if (create && !i)
		if (i=new_block(inode->i_dev)) {
			((unsigned short *) (bh->b_data))[block>>9]=i;
			bh->b_dirt=1;
		}
	brelse(bh);
	if (!i)
		return 0;
	if (!(bh=bread(inode->i_dev,i)))
		return 0;
	i = ((unsigned short *)bh->b_data)[block&511];
	if (create && !i)
		if (i=new_block(inode->i_dev)) {
			((unsigned short *) (bh->b_data))[block&511]=i;
			bh->b_dirt=1;
		}
	brelse(bh);
	return i;
}

int bmap(struct m_inode * inode,int block)
{
	return _bmap(inode,block,0);
}

int create_block(struct m_inode * inode, int block)
{
	return _bmap(inode,block,1);
}

//释放inode，将i_count引用计数减一
//过程中还会有一些额外的判断，如果这个inode代表的是一个块设备文件，那么会同步这个块设备在内存中的缓冲块
//如果这个inode的文件目录项个数为0，那么会先彻底删除这个文件，包括内存中的inode，inode位图，
//	数据块位图（指向索引块与数据块）
//如果inode为脏的话，会先同步inode到设备上
void iput(struct m_inode * inode)
{
	if (!inode)
		return;
	wait_on_inode(inode);
	if (!inode->i_count)
		panic("iput: trying to free free inode");
	if (inode->i_pipe) {//todo 管道处理
		wake_up(&inode->i_wait);
		if (--inode->i_count)
			return;
		free_page(inode->i_size);
		inode->i_count=0;
		inode->i_dirt=0;
		inode->i_pipe=0;
		return;
	}
	//如果设备号是0，inode引用减1（代表是虚拟盘？）
	if (!inode->i_dev) {
		inode->i_count--;
		return;
	}
	//如果是块设备，同步缓冲区与该设备
	if (S_ISBLK(inode->i_mode)) {
		sync_dev(inode->i_zone[0]);
		wait_on_inode(inode);
	}
repeat:
	if (inode->i_count>1) {
		//如果引用数大于1，则减一
		inode->i_count--;
		return;
	}
	if (!inode->i_nlinks) {//如果文件目录项连接数为0，表明没有目录包括这个文件了
		truncate(inode); //将文件内容（文件块与索引块）全部释放
		free_inode(inode); //将inode从inode位图和inode_table中释放
		return;
	}
	if (inode->i_dirt) {//如果inode有修改，那么将inode刷写到设备
		write_inode(inode);	/* we can sleep - so do again */
		wait_on_inode(inode);
		goto repeat;
	}
	inode->i_count--;
	return;
}

//返回inode_table[32]中找到空闲的一项的指针
//“空闲”指的是引用计数为0，且不为脏，没有被锁定
struct m_inode * get_empty_inode(void)
{
	struct m_inode * inode;
	static struct m_inode * last_inode = inode_table;
	int i;

	do {
		inode = NULL;
		for (i = NR_INODE; i ; i--) {
			if (++last_inode >= inode_table + NR_INODE)
				last_inode = inode_table;
			if (!last_inode->i_count) {
				inode = last_inode;
				if (!inode->i_dirt && !inode->i_lock)
					break;
			}
		}
		if (!inode) {
			for (i=0 ; i<NR_INODE ; i++)
				printk("%04x: %6d\t",inode_table[i].i_dev,
					inode_table[i].i_num);
			panic("No free inodes in mem");
		}
		wait_on_inode(inode);
		while (inode->i_dirt) {
			write_inode(inode);
			wait_on_inode(inode);
		}
	} while (inode->i_count);
	memset(inode,0,sizeof(*inode));
	inode->i_count = 1;
	return inode;
}

//找到inode_table中一个空闲的inode，并将它设为管道，这个操作都是内存操作，不会进行任何io
struct m_inode * get_pipe_inode(void)
{
	struct m_inode * inode;

	if (!(inode = get_empty_inode()))
		return NULL;
	if (!(inode->i_size=get_free_page())) { //对于管道inode，i_size字段代表页地址（物理）
		inode->i_count = 0;
		return NULL;
	}
	inode->i_count = 2;	/* sum of readers/writers */
	PIPE_HEAD(*inode) = PIPE_TAIL(*inode) = 0;//i_zone[0] = i_zone[1] = 0,作为管道的头指针和尾指针
	inode->i_pipe = 1; //特殊的标志位，表示是个管道inode
	return inode;
}

//找到设备dev上的第nr个inode，并在内存中设置对应的inode（inode_table[32]），inode内存引用计数加一
//整个定位过程主要是根据已经加载入内存的超级块的inode位图来定位的，然后根据位置发起io从设备读取inode
//注意：这个inode可能是空的，并没有被使用
struct m_inode * iget(int dev,int nr)
{
	struct m_inode * inode, * empty;

	if (!dev)
		panic("iget with dev==0");
	empty = get_empty_inode(); //从inode_table[32]中找一个空闲的inode
	inode = inode_table;
	while (inode < NR_INODE+inode_table) { //遍历inode_table（已打开的inode），看是否存在需要的inode
		if (inode->i_dev != dev || inode->i_num != nr) {
			inode++;
			continue; //如果inode_table被占满的话，这里会一直continue，直到while循环结束
		}
		wait_on_inode(inode);
		if (inode->i_dev != dev || inode->i_num != nr) {
			inode = inode_table;
			continue;
		}
		inode->i_count++;
		if (inode->i_mount) {
			//如果该inode是其它文件系统的安装点，那么搜索已安装的超级块
			//找到对应的超级块，并根据超级块得到挂在的文件系统的根inode
			int i;

			for (i = 0 ; i<NR_SUPER ; i++)
				if (super_block[i].s_imount==inode)
					break;
			if (i >= NR_SUPER) {
				printk("Mounted inode hasn't got sb\n");
				if (empty)
					iput(empty);
				return inode;
			}
			iput(inode);
			dev = super_block[i].s_dev;
			nr = ROOT_INO;//尝试找到挂载的文件系统的根inode
			inode = inode_table;
			continue;
		}
		if (empty)
			iput(empty);
		return inode;//如果需要的inode在inode_table中，那么会从这里返回
	}
	if (!empty) //inode_table[32]没有空闲的inode
		return (NULL);
	inode=empty;
	inode->i_dev = dev;
	inode->i_num = nr;
	read_inode(inode); //从设备上读取inode到inode_table[32]的指定位置
	return inode;//如果是需要的inode不在inode_table中，需要新申请inode，那么会从这里返回
}
//根据设备号和inode号从磁盘上读取inode到指定位置，
//实际上调用这个函数的只有iget方法，并且传入的位置都是inode_table[32]中的位置
//注意，这个inode可能是空的，并没有指向一个文件
static void read_inode(struct m_inode * inode)
{
	struct super_block * sb;
	struct buffer_head * bh;
	int block;

	lock_inode(inode);
	if (!(sb=get_super(inode->i_dev)))
		panic("trying to read inode without dev");
	block = 2 + sb->s_imap_blocks + sb->s_zmap_blocks +
		(inode->i_num-1)/INODES_PER_BLOCK; //根据对应设备的超级块，与inode号，计算inode在设备上的所在文件块
	if (!(bh=bread(inode->i_dev,block))) //根据计算出来的文件块，读取对应的文件块到buffer
		panic("unable to read i-node block");
	*(struct d_inode *)inode =
		((struct d_inode *)bh->b_data)
			[(inode->i_num-1)%INODES_PER_BLOCK]; //将buffer中的数据保存到指定位置（实际上都在inode_table[32]里）
	brelse(bh); //然后释放buffer
	unlock_inode(inode);
}

//将inode_table中的inode写入设备中
//实际上并没有直接写入设备，而是写到了缓冲块中，待缓冲区刷新时写入盘中
static void write_inode(struct m_inode * inode)
{
	struct super_block * sb;
	struct buffer_head * bh;
	int block;

	lock_inode(inode);
	if (!inode->i_dirt || !inode->i_dev) {
		unlock_inode(inode);
		return;
	}
	if (!(sb=get_super(inode->i_dev)))
		panic("trying to write inode without device");
	block = 2 + sb->s_imap_blocks + sb->s_zmap_blocks +
		(inode->i_num-1)/INODES_PER_BLOCK;
	if (!(bh=bread(inode->i_dev,block)))
		panic("unable to read i-node block");
	((struct d_inode *)bh->b_data)
		[(inode->i_num-1)%INODES_PER_BLOCK] =
			*(struct d_inode *)inode;
	bh->b_dirt=1; //代表缓冲区和设备并没有同步,update进程会定时将脏的缓冲块刷入设备
	inode->i_dirt=0; //代表inode和设备同步
	brelse(bh);
	unlock_inode(inode);
}
