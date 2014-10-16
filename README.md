# nginx
A mercurial fork from [http://hg.nginx.org/nginx/](http://hg.nginx.org/nginx/)

*Note:* The mercurial default branch of Nginx is tracked with the **nginx_master** branch of this repo.

### Synchronizing with Nginx repo
To synchronize changesets (commits) from the Nginx mercurial repo, you'll need [hg-git](http://hg-git.github.io) installed.
  
Excute the commands below to get new commits from Nginx into the 
nginx_master branch of this repo:

* git clone &lt;github repo>&gt;
* cd nginx
* mv .git .git2
* hg init
* hg pull http://hg.nginx.org/nginx
* hg update -C default
* hg bookmark -r default nginx_master
* mv .git2 .git
* hg push -B nginx_master git+ssh://<github repo>
* cd ../
* rm -rf nginx

*Note:* for this repo &lt;github repo&gt; is git@github.com:ezbake/nginx.git
