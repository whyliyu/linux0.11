/*
 *  linux/fs/bitmap.c
 *
 *  (C) 1991  Linus Torvalds
 */

/* bitmap.c contains the code that handles the inode and block bitmaps */
#include <string.h>

#include <linux/sched.h>
#include <linux/kernel.h>

#define clear_block(addr) \
__asm__("cld\n\t" \
	"rep\n\t" \
	"stosl" \
	::"a" (0),"c" (BLOCK_SIZE/4),"D" ((long) (addr)):"cx","di")

#define set_bit(nr,addr) ({\
register int res __asm__("ax"); \
__asm__ __volatile__("btsl %2,%3\n\tsetb %%al": \
"=a" (res):"0" (0),"r" (nr),"m" (*(addr))); \
res;})

#define clear_bit(nr,addr) ({\
register int res __asm__("ax"); \
__asm__ __volatile__("btrl %2,%3\n\tsetnb %%al": \
"=a" (res):"0" (0),"r" (nr),"m" (*(addr))); \
res;})

#define find_first_zero(addr) ({ \
int __res; \
__asm__("cld\n" \
	"1:\tlodsl\n\t" \
	"notl %%eax\n\t" \
	"bsfl %%eax,%%edx\n\t" \
	"je 2f\n\t" \
	"addl %%edx,%%ecx\n\t" \
	"jmp 3f\n" \
	"2:\taddl $32,%%ecx\n\t" \
	"cmpl $8192,%%ecx\n\t" \
	"jl 1b\n" \
	"3:" \
	:"=c" (__res):"c" (0),"S" (addr):"ax","dx","si"); \
__res;})

//释放指定设备的指定数据块
//“释放”指的是将该设备超级块的数据块位图中指向这个数据块的位置为0
//如果有对应的缓冲块，也会释放这个缓冲块
//超级块的位图也是保存在缓冲块的，所以将对应缓冲块设为脏
void free_block(int dev, int block)
{
	struct super_block * sb;
	struct buffer_head * bh;

	if (!(sb = get_super(dev)))
		panic("trying to free block on nonexistent device");
	if (block < sb->s_firstdatazone || block >= sb->s_nzones)
		panic("trying to free block not in datazone");
	bh = get_hash_table(dev,block);
	if (bh) {
		if (bh->b_count != 1) {
			printk("trying to free block (%04x:%d), count=%d\n",
				dev,block,bh->b_count);
			return;
		}
		bh->b_dirt=0;
		bh->b_uptodate=0;
		brelse(bh);
	}
	block -= sb->s_firstdatazone - 1 ;
	if (clear_bit(block&8191,sb->s_zmap[block/8192]->b_data)) {
		printk("block (%04x:%d) ",dev,block+sb->s_firstdatazone-1);
		panic("free_block: bit already cleared");
	}
	sb->s_zmap[block/8192]->b_dirt = 1;
}

int new_block(int dev)
{
	struct buffer_head * bh;
	struct super_block * sb;
	int i,j;

	if (!(sb = get_super(dev)))
		panic("trying to get new block from nonexistant device");
	j = 8192;
	for (i=0 ; i<8 ; i++)
		if (bh=sb->s_zmap[i])
			if ((j=find_first_zero(bh->b_data))<8192)
				break;
	if (i>=8 || !bh || j>=8192)
		return 0;
	if (set_bit(j,bh->b_data))
		panic("new_block: bit already set");
	bh->b_dirt = 1;
	j += i*8192 + sb->s_firstdatazone-1;
	if (j >= sb->s_nzones)
		return 0;
	if (!(bh=getblk(dev,j)))
		panic("new_block: cannot get block");
	if (bh->b_count != 1)
		panic("new block: count is != 1");
	clear_block(bh->b_data);
	bh->b_uptodate = 1;
	bh->b_dirt = 1;
	brelse(bh);
	return j;
}

//释放inode
//包括释放inode_table中的位置，即，将inode指向的位置（这个位置肯定在inode_table中）全部置为0
//也包括将inode位图中指向该inode的位设为0
//注意，这里只修改了inode位图，并没有修改数据块位图
void free_inode(struct m_inode * inode)
{
	struct super_block * sb;
	struct buffer_head * bh;

	if (!inode)
		return;
	if (!inode->i_dev) {
		memset(inode,0,sizeof(*inode));
		return;
	}
	if (inode->i_count>1) {
		printk("trying to free inode with count=%d\n",inode->i_count);
		panic("free_inode");
	}
	if (inode->i_nlinks) //说明还有目录包括该文件，不能删除
		panic("trying to free inode with links");
	if (!(sb = get_super(inode->i_dev)))
		panic("trying to free inode on nonexistent device");
	if (inode->i_num < 1 || inode->i_num > sb->s_ninodes)
		panic("trying to free inode 0 or nonexistant inode");
	if (!(bh=sb->s_imap[inode->i_num>>13])) //得到指向这个inode的位图的缓冲块
		panic("nonexistent imap in superblock");
	if (clear_bit(inode->i_num&8191,bh->b_data)) //将位图中指向这个inode的位置为0
		printk("free_inode: bit already cleared.\n\r");
	bh->b_dirt = 1; //将inode位图所在缓冲块设为脏
	memset(inode,0,sizeof(*inode));//将inode指向的位置全部置为0
}

//在指定设备中找到一个空闲的inode，并把它和inode_table中的空闲inode关联
//（通过i_num指向设备中的inode号，通过idirt表明需要刷写inode_table中的inode新数据刷到设备）
struct m_inode * new_inode(int dev)
{
	struct m_inode * inode;
	struct super_block * sb;
	struct buffer_head * bh;
	int i,j;

	if (!(inode=get_empty_inode())) //从inode_table中得到一个空闲的inode位置
		return NULL;
	if (!(sb = get_super(dev))) //得到设备超级块，需要该设备已被挂载
		panic("new_inode with unknown device");
	j = 8192;
	for (i=0 ; i<8 ; i++)	//扫描超级块上的i节点位图，找到空闲inode位置（即节点号）
		if (bh=sb->s_imap[i])
			if ((j=find_first_zero(bh->b_data))<8192)
				break;
	//如果没有找到，或者返回缓冲块无效，则说明没有找到，返回null
	if (!bh || j >= 8192 || j+i*8192 > sb->s_ninodes) {
		iput(inode);
		return NULL;
	}
	if (set_bit(j,bh->b_data)) //在缓冲区中更新i节点位图
		panic("new_inode: bit already set");
	bh->b_dirt = 1;//i节点位图所在缓冲区被设为脏
	inode->i_count=1;//引用计数
	inode->i_nlinks=1;//文件目录项连接数
	inode->i_dev=dev;//inode所在设备号（指向的文件所在的设备号？）
	inode->i_uid=current->euid;//用户
	inode->i_gid=current->egid;//组
	inode->i_dirt=1;//内存中已修改
	inode->i_num = j + i*8192;//设备中的i节点号
	inode->i_mtime = inode->i_atime = inode->i_ctime = CURRENT_TIME;//时间
	return inode;
}
