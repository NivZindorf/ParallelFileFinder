#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>
#include <unistd.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <dirent.h>




//===========================Queue===========================================


struct pnode{
    char * path;
    struct pnode * next;
};

typedef struct pnode Pnode;

struct tnode{
    cnd_t  * cv;
    struct tnode * next;
    thrd_t id;
    int found;//for debug!!!!!!! deleteee!!!
    char * path;
};

typedef struct tnode Tnode;

typedef struct{
    Tnode * head;
    Tnode * tail;
    atomic_int count;
    mtx_t * tmutex;
    cnd_t * maincnd;
    int init;
    int alivecount;
} TQueue;

typedef struct{
    Pnode * head;
    Pnode * tail;
    atomic_int count;
    mtx_t * pmutex;
    cnd_t * maincnd;
    int init;
} PQueue;

typedef struct{
    PQueue * pqueue;
    TQueue * tqueue;
}QBundle;
typedef struct {
    Pnode * pHead;
    Tnode * tHead;
    mtx_t * qmutex;
    int tcount;
    int pcount;
    int threadCount;
    cnd_t * maincnd;
    int init;
} Queue;

PQueue * PQinit(char * rootDir,cnd_t * maincnd){
    PQueue * pqueue = malloc(sizeof(PQueue));
    Pnode * head = malloc(sizeof(Pnode));
    head ->path = rootDir;
    head-> next = NULL;
    pqueue->head = head;
    pqueue->tail = head;
    pqueue->count=1;
    mtx_t * pmutex =malloc(sizeof(mtx_t));
    mtx_init(pmutex,mtx_plain);
    pqueue->pmutex = pmutex;
    pqueue->maincnd = maincnd;
    pqueue->init=1;
    return pqueue;
}
TQueue * TQinit(int maxthreads,cnd_t * maincnd){
    TQueue * tqueue = malloc(sizeof(TQueue));
    tqueue->head = NULL;
    tqueue->tail =NULL;
    tqueue->count=0;
    tqueue->alivecount=maxthreads;
    mtx_t * tmutex = malloc(sizeof(mtx_t));
    mtx_init(tmutex,mtx_plain);
    tqueue->tmutex=tmutex;
    tqueue->maincnd=maincnd;
    tqueue->init=1;
    return tqueue;
}
QBundle * QBinit(PQueue * pqueue,TQueue * tqueue){
    QBundle * qbundle=malloc(sizeof(QBundle));
    qbundle->pqueue=pqueue;
    qbundle->tqueue=tqueue;
    return qbundle;
}


void pushT(TQueue * tqueue, Tnode * tnode){
    mtx_lock(tqueue->tmutex);
    ++(tqueue->count);
    tnode->path=NULL;
    tnode->next=NULL;
    if (tqueue->count == 1){
        tqueue->head = tnode;
        tqueue->tail = tnode;
    }
    else{
        tqueue->tail->next=tnode;
        tqueue->tail=tnode;
    }
    //printf("Added thrd %ld to queue , count = %d\n",(long)(tnode->id),tqueue->count);
    if(tqueue->count == tqueue->alivecount){
        cnd_signal(tqueue->maincnd);
    }
    mtx_unlock(tqueue->tmutex);
}
int popT(TQueue * tqueue){
    mtx_lock(tqueue->tmutex);
    Tnode * head = tqueue->head;
    if (head!=NULL && head->path !=NULL){
        --(tqueue->count);
        tqueue->head = head->next;
        head->next=NULL;
        if(tqueue->count==0){
            tqueue->tail=NULL;
        }
        //printf("POP T FUNC:poped thrd %ld count = %d path %s\n",(long)(head->id),tqueue->count,head->path);
        mtx_unlock(tqueue->tmutex);
        cnd_signal(head->cv);
        return 1;
    }
    else{
        mtx_unlock(tqueue->tmutex);
        return 0;
    }
}
void start(PQueue * pqueue,TQueue * tqueue){
    tqueue->head->path = pqueue->head->path;
    pqueue->head=NULL;
    pqueue->tail=NULL;
    pqueue->count--;
    popT(tqueue);
}
int pop(PQueue *pqueue,TQueue * tqueue){
    mtx_lock(pqueue->pmutex);
    mtx_lock(tqueue->tmutex);
    if(pqueue->count!=0 && tqueue->count !=0){
        Tnode * headt=tqueue->head;
        Pnode * headp=pqueue->head;
        headt->path=headp->path;
        tqueue->head=headt->next;
        headt->next=NULL;
        if(tqueue->count==1){
            tqueue->tail=NULL;
        }
        --(tqueue->count);
        pqueue->head=headp->next;
        if(pqueue->count ==1){
            pqueue->tail=NULL;
        }
        free(headp);
        --(pqueue->count);
        //printf("POP FUNC:pop thred: %ld with path %s, count = %d\n",(long)(headt->id),headt->path,tqueue->count);
        mtx_unlock(tqueue->tmutex);
        mtx_unlock(pqueue->pmutex);
        cnd_signal(headt->cv);
        return 1;
    }
    else{
        mtx_unlock(tqueue->tmutex);
        mtx_unlock(pqueue->pmutex);
        return 0;
    }
}
void pushP(PQueue * pqueue,TQueue * tqueue,char * path){
    mtx_lock(tqueue->tmutex);
    if(tqueue->count>0){
        tqueue->head->path=path;
        mtx_unlock(tqueue->tmutex);
        popT(tqueue);
    }
    else{
        mtx_unlock(tqueue->tmutex);
        mtx_lock(pqueue->pmutex);
        Pnode * pnode=malloc(sizeof(Pnode));
        pnode->path=path;
        pnode->next=NULL;
        ++(pqueue->count);
        if ((pqueue->count)==1){
            pqueue->head=pnode;
            pqueue->tail=pnode;
            cnd_signal(pqueue->maincnd);
        }
        else{
            pqueue->tail->next=pnode;
            pqueue->tail = pnode;
        }
        mtx_unlock(pqueue->pmutex);
    }
    
}


void printThread(TQueue * tqueue){//for debug 
    Tnode * head = tqueue->head;
    while(head != NULL){
        printf("pop thrd %ld from queue\n",(long)(head->id));
        head = head->next;
    }

}

//============================Threads stuff=====================================
atomic_int foundCount = 0;
char * searchterm;
int tryExecute(PQueue * pqueue,TQueue * tqueue,Tnode * tnode){
    //if another path exists,send it to the current thread and return 1,else push thread to tqueue return 0 
    mtx_lock(pqueue->pmutex);
    if (pqueue->count > 0 ){//so there exist one,assign it to current thread
        if (pqueue->count==1){
            tnode->path = pqueue->head->path;
            pqueue->tail=NULL;
            pqueue->head =NULL;
        }
        else{
            tnode->path = pqueue->head->path;
            pqueue->head=pqueue->head->next;
        }
        --(pqueue->count);

        mtx_unlock(pqueue->pmutex);
        return 1;
    }
    else{
        mtx_unlock(pqueue->pmutex);
        pushT(tqueue,tnode);
        return 0;
    }
}
char * concat(char* path,char *name){
    char * newpath=malloc((strlen(path)+strlen(name)+2)*sizeof(char));//+2 becouse of '/' and \0
    int i;
    int firstlen=strlen(path);
    int secondlen=strlen(name);
    for (i=0;i<firstlen;i++){
        newpath[i]=path[i];
    }
    newpath[i++]='/';
    int j;
    for(j=0;j<secondlen;j++){
        newpath[j+i]=name[j];
    }
    newpath[j+i]='\0';
    return newpath;
}

void execute(PQueue * pqueue,TQueue * tqueue,Tnode * tnode){
    char * path = tnode->path;
    DIR * content = opendir(tnode->path);
    struct dirent * entry;
    char * name;
    char * newpath;
    while((entry=readdir(content))!=NULL){//iterating over all entries
        name=entry->d_name;
        if (entry->d_type == DT_DIR){ // so we are reading a directory
            if(!(strcmp(name,".") ==0 || strcmp(name,"..")==0)){
                newpath=concat(path,name);
                if (access(newpath,R_OK|X_OK)<0){
                    printf("Directiory %s: Permission denied.\n",newpath);
                    free(newpath);
                }
                else{
                    pushP(pqueue,tqueue,newpath);
            }
            }
        }
        else{//we are reading a file so check for the search term
            if(strstr(name,searchterm)!=NULL){// inspired from https://stackoverflow.com/questions/15098936/simple-way-to-check-if-a-string-contains-another-string-in-c
            foundCount++;
            tnode->found++;
            printf("%s/%s\n",path,name); //return back to this in the end
            //printf("%s/%s from thrd %ld \n",path,name,(long)(tnode->id));
            }
        }
        }
    tryExecute(pqueue,tqueue,tnode);
}


int threadInit(void * arg){ // 
    QBundle * qbundle =(QBundle *)arg; 
    PQueue * pqueue= qbundle->pqueue;
    TQueue * tqueue= qbundle-> tqueue;
    thrd_t id = thrd_current();
    //printf("Thread %ld started\n",(long)id);
    cnd_t * cv=malloc(sizeof(cnd_t)) ;
    cnd_init(cv);//every thread get his own cv!
    mtx_t * mtx=malloc(sizeof(mtx_t));
    mtx_init(mtx,mtx_plain);//every thread get his own mtx!
    Tnode * tnode = malloc(sizeof(Tnode));
    (tnode -> cv )= cv;
    (tnode->next)=NULL;
    (tnode->id)= id;//Delete this!!!
    (tnode->path)=NULL;
    tnode->found=0;
    pushT(tqueue,tnode);
    mtx_lock(mtx);
    while(1){
        while(tnode->path != NULL){
            execute(pqueue,tqueue,tnode);
        }
        cnd_wait(cv,mtx);       
    }
    printf("!!!!!!!!!!!!!!!!!!!!!!!1thrd %ld exits!!!!!!!!!!!!!!!!!!!!!\n",(long)id);
    thrd_exit(1);
}


//==========================Directories helpers=====================================



int main(int argc,char * argv[]){
    if(argc != 4){
        perror("Error: invalid amount of arguments");
        return 1;
    }
    char * rootDir=argv[1];
    searchterm=argv[2];
    int maxthreads=atoi(argv[3]);

    if (access(rootDir,R_OK|X_OK)<0){
        perror("Error: cant open root directory");
        return 1;
    }
    mtx_t * mainmtx = malloc(sizeof(mtx_t));
    mtx_init(mainmtx,mtx_plain);
    cnd_t * maincnd= malloc(sizeof(cnd_t));
    cnd_init(maincnd);
    PQueue * pqueue = PQinit(rootDir,maincnd);
    TQueue * tqueue=TQinit(maxthreads,maincnd);
    QBundle * qbundle = QBinit(pqueue,tqueue);

    thrd_t * threads  = malloc(((maxthreads)*sizeof(thrd_t)));
    for (int i =0; i<maxthreads ; ++i){
        thrd_create(&threads[i], threadInit ,(void *)qbundle);
    }

    mtx_lock(mainmtx);
    while(tqueue->count<maxthreads){//waiting until all threads finish
        cnd_wait(maincnd,mainmtx);
    }
    /* all threads in queue ,start searching*/
    start(pqueue,tqueue);
    while(!(pqueue->count ==0 && tqueue->count == tqueue->alivecount)){
        while(pop(pqueue,tqueue)){;}
        cnd_wait(maincnd,mainmtx);
    }
    printf("Done searching, found %d files\n",foundCount);
    return 0;
}