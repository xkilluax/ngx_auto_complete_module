# ngx_auto_complete_module

A nginx module that can provide a simple auto complete function, using nginx shared memory and ternary search tree.

# Table of contents
-----
1. [Install](#install)
2. [Usage](#usage)
3. [Dictionary file format description](#dictionary-file-format-description)

##Install
Download the `ngx_auto_complete_module` and uncompress, enter the nginx source file folder,

~~~
./configure --add-module=/path/to/ngx_auto_complete_module
make && make install
~~~

##Usage
Change your nginx config file to include 'auto_complete_dict_path':

~~~
http {
    ...
    server {
        listen 80;
        ...
        location /su {
            auto_complete_dict_path /path/to/dictionary.txt shm_zone=auto_complete:1024m;
        }
        ...
    }
    ...
}
~~~

/path/to/dictionary.txt: auto complete dictionary.

auto_complete: nginx shared memory name.

1024m: the size of nginx shared memory, used for storing dictionary and searching result cache.

test: 

first start the nginx, then

~~~
curl http://localhost/su?s=t
~~~

`JSONP` is also supported

~~~
curl http://localhost/su?s=t&cb=callback
~~~

##Dictionary file format description
dictionary.txt

~~~
1024||nba
100||ternary search tree
0||tst||ternary search tree
...
~~~

"||" is the delimiter, the first column is the weights, the result sort by this value.

The first line means that insert the string "nba", weights is 1024.

The second line means that insert the string "ternary search tree", weights is 100.

The third line means that insert the string "tst", it is an alias of the string "ternary search tree", when searching the string "tst", "ternary search tree" will appear.
