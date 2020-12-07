#include <postgres.h>
#include <limits.h>
#include <libpq/auth.h>
#include <fmgr.h>
#include <fcntl.h>
#include <sched.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include <poll.h>
#include <port.h>
#include <utils/guc.h>
#include <utils/timestamp.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <string.h>
#include <errno.h>
#include <libpq/libpq-be.h>
#include <sys/prctl.h>
#include <stdarg.h>




PG_MODULE_MAGIC;

void            _PG_init(void);
void 			_PG_fini(void);

#define STACK_SIZE (1024 * 1024)
#define errExit(msg)    do { perror(msg); exit(EXIT_FAILURE); \
                        } while (0)


/* Saving the Original Hook */
static ClientAuthentication_hook_type original_client_auth_hook = NULL;
							
/* GUC variables */
static char *pgsilo_root_dir = NULL;        /* Define a temp directory to be used as a base for new roots path */
static char *pgsilo_custom_fs= NULL;        /* Define a the list of white listed directory for every database */

static int nb_silo;
static int pg_gid,pg_uid;

static struct pgsilo_ns_conf
{
    int pid;
    char dbname[50]; 
    char custom_fs[PATH_MAX];
};

static struct pgsilo_ns_conf arr_pgsilo_ns_conf[10];

static int pivot_root(const char *new_root, const char *put_old)
{
    return syscall(SYS_pivot_root, new_root, put_old);
}

/* source https://gist.github.com/JonathonReinhart/8c0d90191c38af2dcadb102c4e202950 */
static int mkdir_p(const char *path)
         {
                 /* Adapted from http://stackoverflow.com/a/2336245/119527 */
                 const size_t len = strlen(path);
                 char _path[PATH_MAX];
                 char *p;

                 errno = 0;

                 /* Copy string so its mutable */
                 if (len > sizeof(_path)-1) {
                         errno = ENAMETOOLONG;
                         return -1;
                 }
                 strcpy(_path, path);

                 /* Iterate the string */
                 for (p = _path + 1; *p; p++) {
                         if (*p == '/') {
                                 /* Temporarily truncate */
                                 *p = '\0';

                                 if (mkdir(_path, S_IRWXU) != 0) {
                                         if (errno != EEXIST)
                                                 return -1;
                                 }

                                 *p = '/';
                         }
                 }

                 if (mkdir(_path, S_IRWXU) != 0) {
                         if (errno != EEXIST)
                                 return -1;
                 }

                 return 0;
         }



static void write_file(char path[PATH_MAX], char user_map_line[100])
{
    FILE *f = fopen(path, "w");

    if (f == NULL) {
      errExit("write_file-open");
    }

    if (fwrite(user_map_line, 1, strlen(user_map_line), f) < 0) {
      errExit("write_file_write");
    }

    if (fclose(f) != 0) {
      errExit("write_file-close");
    }
}

static int new_namespace(struct pgsilo_ns_conf *arg) /* Startup a new user mount namespace  */
{
  
    char new_root[PATH_MAX] ;
    const char *put_old = "/oldrootfs";
    char path[PATH_MAX];
    char source[PATH_MAX];
    char dest[PATH_MAX];
    char flag[10];
	int ns_pid=getpid();
	char user_map_line[100];
	FILE* ptr = fopen(arg->custom_fs,"r");
	
	if (ptr==NULL)
          errExit("mount_namespace-open");
    
	
	/*Configure the user namespace */
	
	sprintf(path, "/proc/%d/uid_map", ns_pid);
    sprintf(user_map_line, "%d %d 1\n", pg_uid,pg_uid);
    write_file(path, user_map_line);

    sprintf(path, "/proc/%d/setgroups", ns_pid);
    sprintf(user_map_line, "deny");
    write_file(path, user_map_line);

    sprintf(path, "/proc/%d/gid_map", ns_pid);
    sprintf(user_map_line, "%d %d 1\n", pg_gid,pg_gid);
    write_file(path, user_map_line);
	
	/*Configure the mount namespace */    
	
	snprintf(new_root, sizeof(new_root), "%s/%s", pgsilo_root_dir, arg->dbname);
	
	
    /*Changing the process name */ 
	snprintf(path, sizeof(path), "pgsilo %s", arg->dbname);
    if ( prctl(PR_SET_NAME,path,NULL,NULL,NULL) == -1)
		 errExit("PR_SET_NAME");;
	
	 if (mkdir_p(new_root) == -1)
                     errExit("mkdir");

    /* Ensure that 'new_root' and its parent mount don't have
       shared propagation (which would cause pivot_root() to
       return an error), and prevent propagation of mount
       events to the initial mount namespace */

    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) == 1)
       errExit("mount-MS_PRIVATE");  
  

    /* Ensure that 'new_root' is a mount point */

    if (mount(new_root, new_root, NULL, MS_BIND, NULL) == -1)
        errExit("mount-MS_BIND");

    /* Mount the new white listed directory in the new root directory */ 
    while (fscanf(ptr,"%s %s %s ",source,dest,flag)==3)
     {
              
                snprintf(path, sizeof(path), "%s/%s", new_root, dest);
                if (mkdir_p(path) == -1)
                        errExit("mkdir");
					
				if (strcmp(flag,"ro") == 0)
				{
					if (mount(source,path, NULL, MS_BIND | MS_RDONLY, NULL) == -1)
						errExit("mount-MS_BIND-MS_RDONLY");					
				}
				  else
				{
					if (mount(source,path, NULL, MS_BIND, NULL) == -1)
						errExit("mount-MS_BIND");
				} 
                
     }	 
	 
					
    /* Create directory to which old root will be pivoted */

    snprintf(path, sizeof(path), "%s/%s", new_root, put_old);
    if (mkdir(path, 0777) == -1)
        errExit("mkdir");

    /* And pivot the root filesystem */

    if (pivot_root(new_root, path) == -1)
        errExit("pivot_root");
	
    /* Switch the current working directory to "/" */
    if (chdir("/") == -1)
        errExit("chdir");

    /* Unmount old root and remove mount point */

    if (umount2(put_old, MNT_DETACH) == -1)
        perror("umount2");
    if (rmdir(put_old) == -1)
        perror("rmdir");	


    /* Put the process to sleep to maintain the namespace open  */
     pause();
 }


static void enter_ns(Port *port, int status) /* enter the specified namespace */
{
	
	    char userns[PATH_MAX];
        char mntns[PATH_MAX];
		char* pg_data = getenv("PGDATA");
		int i; 
		int fd1;
        int fd2;
		
        /*
         * Any other plugins which use ClientAuthentication_hook.
         */
        if (original_client_auth_hook)
                original_client_auth_hook(port, status);

      
        if (status != STATUS_OK)
        {
                return;
        }		
		
		/* set the target silo to the dafault one supposed to be a at slot 0 */ 
		snprintf(userns, sizeof(userns), "/proc/%d/ns/user",  arr_pgsilo_ns_conf[0].pid);
        snprintf(mntns, sizeof(mntns), "/proc/%d/ns/mnt",  arr_pgsilo_ns_conf[0].pid);
		
		  /* Check if there is a custom silo for the target database*/ 	 
		  for (i = 0; i < nb_silo; i++) 
		  {  		
		   if (strcmp(port->database_name,arr_pgsilo_ns_conf[i].dbname) == 0)
		   {
			snprintf(userns, sizeof(userns), "/proc/%d/ns/user", arr_pgsilo_ns_conf[i].pid);
            snprintf(mntns, sizeof(mntns), "/proc/%d/ns/mnt",  arr_pgsilo_ns_conf[i].pid);
		   }
		  }

        fd1 = open(userns,O_RDONLY);
		if (fd1==NULL)
          errExit("enter_ns-open_file");
        fd2 = open(mntns,O_RDONLY);
	    if (fd2==NULL)
          errExit("enter_ns-open_file");

        /* Join the user and mount namespace */ 
		if (setns(fd1, 0) == -1)
           perror("setns");
	    if (setns(fd2, 0) == -1)
           perror("setns");

        /* restore the current directory to PG_DATA */ 
		if (chdir(pg_data) == -1)
           perror("setns");
        

}

/*
 * Module Load Callback
 */
void _PG_init(void)
{
	
	char fs_layout_path[PATH_MAX];
	char root_path[PATH_MAX];
    char dbname[50];
	int i = 0;
	char *stack;
	
	
		/* Define custom GUC variables */
	 DefineCustomStringVariable("pgsilo.base_root_dir",
								"Define a temp directory to be used as a base for new roots path ",
								NULL,
								&pgsilo_root_dir,
								"/tmp/bin",
								PGC_SUSET,
								GUC_NOT_IN_SAMPLE,						
								NULL,
								NULL,
								NULL);
								
	 DefineCustomStringVariable("pgsilo.custom_fs_layout",
								"Define custom fs layout per database (default,db1,db2) ",
								NULL,
								&pgsilo_custom_fs,
								"",
								PGC_SUSET,
								GUC_NOT_IN_SAMPLE,						
								NULL,
								NULL,
								NULL);							
        /* Install Hooks */
        original_client_auth_hook = ClientAuthentication_hook;
        ClientAuthentication_hook = enter_ns;
	
    pg_uid=getuid();
	pg_gid=getgid();
		
		
    FILE* ptr = fopen(pgsilo_custom_fs,"r");

    if (ptr==NULL)
          errExit("_PG_init-open_file");
	
	while (fscanf(ptr,"%s %s",dbname,fs_layout_path)==2)
     {                               			
				strcpy(arr_pgsilo_ns_conf[i].dbname,dbname);
				strcpy(arr_pgsilo_ns_conf[i].custom_fs,fs_layout_path);			
				
				      /* Create a child process in a user and new mount namespace */

				stack = mmap(NULL, STACK_SIZE, PROT_READ | PROT_WRITE,
								   MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
				if (stack == MAP_FAILED)
					errExit("mmap");
				
				arr_pgsilo_ns_conf[i].pid=clone(new_namespace, stack + STACK_SIZE,CLONE_NEWUSER |CLONE_NEWNS | SIGCHLD, (void *)(&arr_pgsilo_ns_conf[i]));
				if (arr_pgsilo_ns_conf[i].pid == -1)
					errExit("clone");	
				i++;               
				elog(WARNING,"Silo created");				
     }
	 nb_silo=i;

}

void _PG_fini(void)
{	
  int i; 
  /* close the namespaces */ 	 
  for (i = 0; i < nb_silo; i++) 
  {  
    kill(arr_pgsilo_ns_conf[i].pid, SIGHUP);
  }
}	
