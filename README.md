# nginx
A mercurial fork from [http://hg.nginx.org/nginx/](http://hg.nginx.org/nginx/)

*Note:* The mercurial default branch of Nginx is tracked with the **nginx_master** branch of this repo.

### Synchronizing with Nginx repo
To synchronize changesets (commits) from the Nginx mercurial repo, you'll need [hg-git](http://hg-git.github.io) installed.
  
Excute the commands below to get new commits from Nginx into the 
nginx_master branch of this repo:

```
$ git clone git@github.com:ezbake/nginx.git
$ cd nginx
$ mv .git .git2
$ hg init
$ hg pull http://hg.nginx.org/nginx
$ hg branches | awk '{cmd="hg update -C "$1;print "-> "cmd;system(cmd)}'
$ hg branches | awk '{cmd="hg bookmark -r "$1" nginx_"$1;print "-> "cmd;system(cmd)}'
$ mv .git2 .git
$ hg branches | awk '{cmd="hg push -B nginx_"$1" git+ssh://git@github.com:ezbake/nginx.git";print "-> "cmd;system(cmd)}'
$ cd ../
$ rm -rf nginx
```

