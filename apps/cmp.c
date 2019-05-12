#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>

#define BLOCKSIZE 4096

typedef struct merkel_tree{
    struct merkel_tree* l;
    struct merkel_tree* r;
    int block;
    int hash;
    int depth;
}merkel_tree;
#define EXT4_IOC_GETTREE _IOWR('f',22, struct merkel_tree)

typedef struct byteDiffLinked{
    int pos;
    char a;
    char b;
    struct byteDiffLinked* next;
}byteDiffLinked;

//------------------------------------
//------------------------------------
//		Utils
//------------------------------------
//------------------------------------
int max(int a, int b)
{
    return (a>b)?a:b;
}

int getPadding(int maxVal)
{
    int pad = 0;
    for(;maxVal != 0;maxVal/=10)    
 	pad++;
    return pad;
}

char* getShortestFile(char* fileA, char* fileB)
{    
    //open file A
    FILE* fdA = fopen(fileA, "r");
    if (!fdA)
    {
        perror("open");
        return NULL;
    }
    fseek(fdA, 0, SEEK_END);
    int posA = ftell(fdA);
    fclose(fdA);
        
    //open file B
    FILE* fdB = fopen(fileB, "r");
    if (!fdB)
    {
        perror("open");
        return NULL;
    }
    fseek(fdB, 0, SEEK_END);
    int posB = ftell(fdB);
    fclose(fdB);
    
    //return shortest
    if(posA>posB)
        return fileB;
    else if(posB>posA)
        return fileA;
    else 
        return NULL;
}

void freeMerkelTree(merkel_tree* tree)
{
    if(tree->block < 0)
        freeMerkelTree(tree->l);
    if(tree->block == -2)
        freeMerkelTree(tree->r);
    free(tree);
}

//------------------------------------
//------------------------------------
//          GETTING FILE DATA
//------------------------------------
//------------------------------------

merkel_tree* getNode(int fd,int path,int depth)
{
    //init
    merkel_tree* tmp = malloc(sizeof(merkel_tree));
    tmp->depth = depth;
    tmp->block = path;        

    //get node
    int err;
    if ((err = ioctl(fd, EXT4_IOC_GETTREE, tmp)))
    {
        close(fd);
        fprintf(stderr,"error ioctl : %d\n",err);
        exit(EXIT_FAILURE);
    }   

    return tmp;
}

void getFileTreeBis(int fd, merkel_tree* tree, int path, int mult, int depth)
{
    //check if leaf
    if(tree->block >= 0)
        return;
    
    //get left child
    tree->l = getNode(fd,path, depth);
    getFileTreeBis(fd, tree->l, path, mult*2, depth-1);

    //get right child if exist
    if(tree->block != -2)
    {
	tree->r = NULL;
	return;
    }
    tree->r = getNode(fd,path+mult, depth);
    getFileTreeBis(fd, tree->r, path+mult, mult*2, depth-1);
}

merkel_tree* getFileTree(char* filename)
{
    //open file
    int fd = open(filename, O_RDWR);
    if (fd < 0)
    {
        fprintf(stderr,"error opening file %s : %d\n",filename,fd);
        exit(EXIT_FAILURE);
    }

    //get tree
    merkel_tree* root = getNode(fd,0,-1);
    getFileTreeBis(fd, root, 0, 1, root->depth-1);
    close(fd);
    return root;
}


char* getBlock(char* filename,int blknb)
{
    //open file
    FILE* fd = fopen(filename, "r");
    if (!fd) 
    {
        perror("open");
        return NULL;
    }
    
    //get block
    char ch;
    char* str = malloc(sizeof(char)*(BLOCKSIZE+1));
    int i;
    fseek(fd, blknb*BLOCKSIZE, SEEK_SET);
    for(i=0;(ch = fgetc(fd)) != EOF && i<BLOCKSIZE;i++)
        str[i] = ch;
    fclose(fd);
    str[i] = '\0';
    return str;
}

//----------------------------------------------------
//----------------------------------------------------
//          byteDiffLinked operations
//----------------------------------------------------
//----------------------------------------------------

byteDiffLinked* byteDiffLinked_create(int pos,char a,char b)
{
    byteDiffLinked* ret = malloc(sizeof(byteDiffLinked));
    ret->pos  = pos;
    ret->a    = a;
    ret->b    = b;
    ret->next = NULL;
    
    return ret;
}

byteDiffLinked* byteDiffLinked_append(byteDiffLinked* root, byteDiffLinked* node)
{
    if(!root)
        return node;
    
    byteDiffLinked* it;
    for(it = root;it->next != NULL;it = it->next);        
    it->next = node;
    
    return root;
}



//----------------------------------------
//----------------------------------------
//          comparison operations
//----------------------------------------
//----------------------------------------

byteDiffLinked* compareStrings(char* str1,char* str2,int blknb)
{
    byteDiffLinked* ret = NULL;
    for(int i=0;i<BLOCKSIZE;i++)
    {
        //be waware that file may not be the same size
        if((str1[i] == '\0') || (str2[i] == '\0'))
            break;
        
        //if differ => add a ByteDiffLinkedNode
        if(str1[i] != str2[i])
        {
            byteDiffLinked* bdln = byteDiffLinked_create(1+i+(blknb*BLOCKSIZE),str1[i],str2[i]);
            ret = byteDiffLinked_append(ret,bdln);
        }        
    }
    
    return ret;
}

byteDiffLinked* getByteDiff(char* file1,char* file2,merkel_tree* t1,merkel_tree* t2)
{   
    //check same root hash
    if(t1->hash == t2->hash)
        return NULL;   
        
    //if leaves, get difference and return
    if(t1->block == t2->block && t2->block >= 0)
    {
       
        char* str1 = getBlock(file1, t2->block);
        char* str2 = getBlock(file2, t2->block);
        byteDiffLinked* res = compareStrings(str1, str2, t2->block);
        
        free(str1);
        free(str2);

        return res;        
    }    
   
    //else go deeper
    byteDiffLinked* ret = NULL;
    if(t1->l && t2->l)
        ret = byteDiffLinked_append(ret,getByteDiff(file1, file2, t1->l, t2->l));
    if(t1->r && t2->r)
        ret = byteDiffLinked_append(ret,getByteDiff(file1, file2, t1->r, t2->r));
    
    return ret;
}

//--------------------
//--------------------
//        main
//--------------------
//--------------------

int main (int argc, char *argv[])
{
    //check arguments
    if(argc != 4 || strcmp(argv[1],"-l"))
    {
        printf("Missing arguments expected custom_cmp -l <file1> <file2>\n");
        return -1;
    }
   
    //get trees
    merkel_tree* t1 = getFileTree(argv[2]);
    merkel_tree* t2 = getFileTree(argv[3]);
    while(t2->depth > t1->depth)
        t2 = t2->l;
    while(t1->depth > t2->depth)
        t1 = t1->l;
    
    //compare tree
    byteDiffLinked* res = getByteDiff(argv[2], argv[3], t1, t2);
    
    //get padding
    int maxPos = 0, maxA = 0, maxB = 0;
    if(res)
    {
        byteDiffLinked* it = res;
        while(it)
        {            
	    maxPos = max(maxPos, it->pos);
            maxA   = max(maxA, it->a);
            maxB   = max(maxB, it->b);
            it     = it->next;           
        }
    }
    int padPos = getPadding(maxPos);
    int padA   = getPadding(maxA);
    int padB   = getPadding(maxB);
    

    //if diff exist, print out and free linked list	
    if(res)  
    {        
        byteDiffLinked* it = res;
        while(it)     
        {               
            printf("%*d %*o %*o\n", padPos, it->pos, padA, it->a, padB, it->b);
            
            byteDiffLinked* toFree = it;
            it = it->next;
            free(toFree);
        }
    }
    
    //display EOF message if needed
    char* shortest = getShortestFile(argv[2], argv[3]);
    if(shortest)
        printf("cmp: EOF on %s\n", shortest);
    
    //free and quit    
    freeMerkelTree(t1);
    freeMerkelTree(t2);
    return 0;
}

