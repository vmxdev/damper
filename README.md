# damper
Linux userspace traffic shaper

Utility for shaping network traffic. `damper` uses NFQUEUE mechanism for capture traffic and raw sockets for re-injecting. All processing occurs in user space as opposed to traditional kernel space Linux traffic shaping.

### Compiling

(netfilter-queue library required)

```sh
$ cc -Wall -pedantic damper.c modules.conf.c -o damper -lnetfilter_queue -pthread -lrt -lm
```

### Running on local box

For shaping outgoing locally generated TCP traffic

First allow reinjected traffic. It prevents the same network packet to be passed to shaper again. Reinjected packets can be marked in shaper ("mark" directive in damper.conf).

```sh
# iptables -t raw -A OUTPUT -m mark --mark 88 -j ACCEPT
```

Or if you want to change DSCP value in reinjected packets instead of using mark ("2" is new packets DSCP, "dscp" directive in config):

```sh
# iptables -t raw -A OUTPUT -m dscp --dscp 2 -j ACCEPT
```

Next select traffic for shaping. For example with this rule all locally-generated TCP traffic will be sent to shaper (which is listening on queue number 3, "queue" in config).

```sh
# iptables -t raw -A OUTPUT -p tcp -j NFQUEUE --queue-num 3
```

Packets will be dropped and reinjected in another order (based on shaper packets priority) on interface from damper.conf, "iface eth0" for example. In case of overlimit packets with lowest priority will not be sent (completely dropped).

And here is rules for shaping incoming TCP traffic on interface eth0.

```sh
 (accept reinjected packets)
# iptables -t raw -A PREROUTING -m mark --mark 88 -j ACCEPT
 (pass all TCP on eth0 to shaper)
# iptables -t raw -A PREROUTING -i eth0 -p tcp -j NFQUEUE --queue-num 3
```

To make it work put "iface lo" in config, reinjected packets will be generated on loopback interface

### Running on router (shaping forwarded traffic)



### Statistics

damper comes with web-based statistics viewer, demo available here: http://damper.xenoeye.com

Chart displayed using SCGI module. It can be integrated with Apache, Nginx, lighthttp or any web-server which support SCGI interface

To compile module:

```sh
$ cd stat
$ cc -g -Wall -pedantic damper_img.c -o damper_img -lpng -pthread
```

Now you can run it and configure http-server

```sh
$ ./damper_img 9001 /var/lib/damper/ &
```

Sample Nginx configuration:

```
server {
    server_name MY.SERVER.NAME;
    root /var/www/MY.LOCATION; 

   # images
   location /damper-img {
        include scgi_params;
        scgi_pass 127.0.0.1:9001;
        scgi_param SCRIPT_NAME "/damper-img";
   }

    location / {
        try_files $uri $uri/index.html index.html;
    }

}
```

For Apache add this string to <VirtualHost>:

```
SCGIMount /damper-img 127.0.0.1:9001
```
