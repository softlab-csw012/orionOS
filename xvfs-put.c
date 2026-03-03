#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define BLOCK_SIZE 512
#define XVFS_MAGIC 0x58564653
#define NAME_LEN 16

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

/* ---------------- raw block IO ---------------- */

void read_block(uint32_t lba, void *buf){
    fseek(disk, (lba + base_lba)*BLOCK_SIZE, SEEK_SET);
    fread(buf,1,BLOCK_SIZE,disk);
}

void write_block(uint32_t lba, void *buf){
    fseek(disk, (lba + base_lba)*BLOCK_SIZE, SEEK_SET);
    fwrite(buf,1,BLOCK_SIZE,disk);
}

/* ---------------- detect partition ---------------- */

void detect_partition(){
    uint8_t mbr[512];
    fseek(disk,0,SEEK_SET);
    fread(mbr,1,512,disk);

    if(mbr[510]==0x55 && mbr[511]==0xAA){
        base_lba = *(uint32_t*)(mbr+454);
    }else base_lba=0;
}

/* ---------------- bitmap alloc ---------------- */

uint32_t alloc_block(){
    uint8_t buf[512];
    uint32_t bits = BLOCK_SIZE*8;

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

/* ---------------- dir lookup ---------------- */

int find_in_dir(uint32_t dir,const char *name,Entry *out){
    Entry ents[BLOCK_SIZE/sizeof(Entry)];
    read_block(dir,ents);

    size_t len = strlen(name);
    if(len >= NAME_LEN) return -1;

    for(int i=0;i<BLOCK_SIZE/sizeof(Entry);i++){
        if(ents[i].name[0]==0 || (uint8_t)ents[i].name[0]==0xE5) continue;

        if(memcmp(ents[i].name, name, len)==0 &&
           ents[i].name[len]==0)
        {
            if(out)*out=ents[i];
            return i;
        }
    }
    return -1;
}

int free_slot(uint32_t dir){
    Entry ents[BLOCK_SIZE/sizeof(Entry)];
    read_block(dir,ents);

    for(int i=0;i<BLOCK_SIZE/sizeof(Entry);i++)
        if(ents[i].name[0]==0 || (uint8_t)ents[i].name[0]==0xE5) return i;

    return -1;
}

/* ---------------- path traversal ---------------- */

uint32_t resolve_dir(const char *path,char *filename){
    char temp[256];
    strcpy(temp,path);

    uint32_t cur=sb.root_dir_block;
    char *tok=strtok(temp,"/");

    while(tok){
        char *next=strtok(NULL,"/");
        if(!next){
            strcpy(filename,tok);
            return cur;
        }

        Entry e;
        if(find_in_dir(cur,tok,&e)<0 || !(e.attr&1)){
            printf("no such dir: %s\n",tok);
            exit(1);
        }

        cur=e.start;
        tok=next;
    }
    return cur;
}

/* ---------------- write file ---------------- */

void write_file(uint32_t dir,const char *name,FILE *src){
    Entry ents[BLOCK_SIZE/sizeof(Entry)];
    read_block(dir,ents);

    int slot=free_slot(dir);
    if(slot<0){ printf("dir full\n"); exit(1); }

    fseek(src,0,SEEK_END);
    uint32_t size=ftell(src);
    rewind(src);

    uint8_t buf[512];
    uint32_t first=0,prev=0;

    while(1){
        size_t r=fread(buf,1,508,src);
        if(!r)break;

        uint32_t b=alloc_block();
        if(!b){ printf("disk full\n"); exit(1); }

        uint8_t block[512]={0};
        memcpy(block+4,buf,r);

        if(prev){
            uint8_t tmp[512];
            read_block(prev,tmp);
            *(uint32_t*)tmp=b;
            write_block(prev,tmp);
        }else first=b;

        write_block(b,block);
        prev=b;
    }

    if(prev){
        uint8_t tmp[512];
        read_block(prev,tmp);
        *(uint32_t*)tmp=0;
        write_block(prev,tmp);
    }

    memset(&ents[slot],0,sizeof(Entry));
    memset(ents[slot].name,0,16);
    strncpy(ents[slot].name,name,15);
    ents[slot].start=first;
    ents[slot].size=size;
    ents[slot].attr=0;
    ents[slot].owner=0;
    ents[slot].mode=0x03;

    write_block(dir,ents);
}

/* ---------------- main ---------------- */

int main(int argc,char**argv){
    if(argc!=4){
        printf("usage: xvfs-put <img> <hostfile> </path>\n");
        return 1;
    }

    disk=fopen(argv[1],"r+b");
    if(!disk){ perror("open img"); return 1; }

    detect_partition();

    read_block(1,&sb);
    if(sb.magic!=XVFS_MAGIC){
        printf("not xvfs\n");
        return 1;
    }

    FILE *src=fopen(argv[2],"rb");
    if(!src){ perror("open src"); return 1; }

    char fname[64];
    uint32_t dir=resolve_dir(argv[3],fname);

    write_file(dir,fname,src);

    printf("put %s -> %s OK\n",argv[2],argv[3]);
    return 0;
}
