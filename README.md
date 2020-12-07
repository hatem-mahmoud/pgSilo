
pgSilo Extension : Enhance your PostgreSQL security and isolation
===================================

PgSilo is a new PostgreSQL extension that aim to provide better security and isolation by confining PostgreSQL back-end session into silo. Every PostgreSQL cluster will be split into many silo, we can have at must one silo per database, this aim to provide better native security when deploying PostgreSQL. A compromised process connected to database A  (Silo A) will not be able to affect database B (Silo B)  in the same cluster or in another database cluster on same HOST machine. That's the ultimate objective , but we are still far from that!

pgSilo is still in active development and there is still a lot to do but I decided to share it at the early stage to get feedback and contribution of course . Here is a glimpse of what the actual Pre-APLHA release can do :

![alt text](https://mahmoudhatem.files.wordpress.com/2019/03/capture-17.png)

Essentially, at this stage pgsilo allow back-end process connected to different silo (database) to have a different view of the file system layout, which aim to provide fs access control and isolation. Pgsilo rely on Linux namespace for that. The back-end process are bound to different child user and mount namespaces. The new FS layout is constructed using a combination of Bind mount and pivot root.


Installation
============

Make sure that you have the PostgreSQL headers and the extension
building infrastructure installed.  If you do not build PostgreSQL
from source, this is done by installing a `*-devel` or `*-dev`
package.

Check that the correct `pg_config` is found on the `PATH`.  
Then build and install `pgsilo` with

     make  USE_PGXS=1 && make  USE_PGXS=1 install

Then you must add `pgsilo` to `shared_preload_libraries` and restart
the PostgreSQL server process, but make sure that you have completed the
setup as described below, or PostgreSQL will not start.


Setup
=====


Here is the basic configuration 

- pgsilo.base_root_dir : Define a temporary directory to be used as a base for building the new roots path
- pgsilo.custom_fs_layout: Define custom fs layout per database (default (required),db1,db2)



The FS layout is defined using a whitelist configuration file.

Example: custom_fs_layout.txt

Default /path_to_the_withlist_file_list
Db_name /path_to_the_withlist_file_list2
Db_name2 /path_to_the_withlist_file_list3

Example : /path_to_the_withlist_file_list

Source_directory destination_directory flag(ro|rw)

Demo 
=====


To DO
=====


As stated there still many thing to DO in the current release :

- Integration with selinux (The test are done with selinux set to permissive) 
- Creating a default and minimal FS layout
- Code review and enhancement
- Regression test

And here is some idea for future development :

- Integration with Linux cgroup (There is already pgcrgoup we could integrate it)
- Integration with seccomp (There is already pgseccomp we cloud integrate it)
- Integrating with other namespace (Example : Pid/Network namespaces) 
 

