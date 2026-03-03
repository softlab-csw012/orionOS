#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define BLOCK_SIZE 512
#define XVFS_MAGIC 0x58564653

typedef struct {
    uint32_t magic;
    uint32_t block_size;
    uint32_t total_blocks;
    uint32_t bitmap_start;
    uint32_t data_start;
    uint32_t free_blocks;
    uint32_t root_dir_block;
} __attribute__((packed)) Super;

typedef struct {
    char name[16];
    uint32_t start;
    uint32_t size;
    uint8_t attr;
    uint32_t owner;
    uint8_t mode;
} __attribute__((packed)) Entry;

static FILE *disk;
static uint32_t base_lba = 0;
static Super sb;

/* ---------------- block IO ---------------- */

void read_block(uint32_t lba, void *buf){
    fseek(disk,(lba+base_lba)*BLOCK_SIZE,SEEK_SET);
    fread(buf,1,BLOCK_SIZE,disk);
}

void write_block(uint32_t lba, void *buf){
    fseek(disk,(lba+base_lba)*BLOCK_SIZE,SEEK_SET);
    fwrite(buf,1,BLOCK_SIZE,disk);
}

/* ---------------- partition detect ---------------- */

void detect_partition(){
    uint8_t mbr[512];
    fseek(disk,0,SEEK_SET);
    fread(mbr,1,512,disk);

    if(mbr[510]==0x55 && mbr[511]==0xAA)
        base_lba=*(uint32_t*)(mbr+454);
    else
        base_lba=0;
}

/* ---------------- bitmap alloc ---------------- */

uint32_t alloc_block(){
    uint8_t buf[512];
    uint32_t bits=BLOCK_SIZE*8;

    for(uint32_t b=sb.data_start;b<sb.total_blocks;b++){
        uint32_t blk=b/bits,bit=b%bits;
        read_block(sb.bitmap_start+blk,buf);

        if(!(buf[bit/8]&(1<<(bit%8)))){
            buf[bit/8]|=(1<<(bit%8));
            write_block(sb.bitmap_start+blk,buf);
            return b;
        }
    }
    return 0;
}

/* ---------------- dir helpers ---------------- */

int find_entry(uint32_t dir,const char *name,Entry *out){
    Entry e[BLOCK_SIZE/sizeof(Entry)];
    read_block(dir,e);

    for(int i=0;i<BLOCK_SIZE/sizeof(Entry);i++){
            if(e[i].name[0] && (uint8_t)e[i].name[0] != 0xE5 &&
                strlen(name) < 16 &&
                memcmp(e[i].name, name, strlen(name)) == 0 &&
                e[i].name[strlen(name)] == 0)
            {
            if(out)*out=e[i];
            return i;
        }
    }
    return -1;
}

int free_slot(uint32_t dir){
    Entry e[BLOCK_SIZE/sizeof(Entry)];
    read_block(dir,e);

    for(int i=0;i<BLOCK_SIZE/sizeof(Entry);i++)
        if(e[i].name[0]==0 || (uint8_t)e[i].name[0]==0xE5)return i;
    return -1;
}

/* ---------------- path traversal ---------------- */

uint32_t resolve_parent(const char *path,char *dirname){
    char tmp[256];
    strcpy(tmp,path);

    uint32_t cur=sb.root_dir_block;
    char *tok=strtok(tmp,"/");

    while(tok){
        char *next=strtok(NULL,"/");

        if(!next){
            strcpy(dirname,tok);
            return cur;
        }

        Entry e;
        if(find_entry(cur,tok,&e)<0 || !(e.attr&1)){
            printf("no such dir: %s\n",tok);
            exit(1);
        }

        cur=e.start;
        tok=next;
    }
    return cur;
}

/* ---------------- mkdir ---------------- */

void make_dir(uint32_t parent,const char *name){
    Entry ents[BLOCK_SIZE/sizeof(Entry)];
    read_block(parent,ents);

    if(find_entry(parent,name,NULL)>=0){
        printf("already exists: %s\n",name);
        return;
    }

    int slot=free_slot(parent);
    if(slot<0){
        printf("directory full\n");
        exit(1);
    }

    uint32_t blk=alloc_block();
    if(!blk){
        printf("disk full\n");
        exit(1);
    }

    /* 새 디렉토리 블록 초기화 */
    uint8_t empty[512]={0};
    write_block(blk,empty);

    memset(&ents[slot],0,sizeof(Entry));
    memset(ents[slot].name,0,16);
    strncpy(ents[slot].name,name,15);
    ents[slot].start=blk;
    ents[slot].attr=1;
    ents[slot].owner=0;
    ents[slot].mode=0x03;

    write_block(parent,ents);

    printf("mkdir %s block=%u\n",name,blk);
}

/* ---------------- main ---------------- */

int main(int argc,char**argv){
    if(argc!=3){
        printf("usage: xvfs-mkdir <img> </dir>\n");
        return 1;
    }

    disk=fopen(argv[1],"r+b");
    if(!disk){perror("open");return 1;}

    detect_partition();

    read_block(1,&sb);
    if(sb.magic!=XVFS_MAGIC){
        printf("not xvfs\n");
        return 1;
    }

    char name[64];
    uint32_t parent=resolve_parent(argv[2],name);

    make_dir(parent,name);

    return 0;
}
