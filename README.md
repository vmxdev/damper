# damper
Linux userspace traffic shaper

Utility for shaping network traffic. `damper` uses NFQUEUE mechanism for capture traffic and raw sockets for re-injecting. All processing occurs in user space as opposed to traditional kernel space Linux traffic shaping.

### Compiling

(netfilter-queue library required)

```sh
$ cc -Wall -pedantic damper.c modules.conf.c -o damper -lnetfilter_queue -pthread -lrt -lm
```

### Shaping and modules

`damper` works approximately in this way: at startup two threads are created. First thread captures network packets (via NFQUEUE), calculate "weight" or priority for each one and put it in priority queue. Whet queue is full, packets with low priority replaced with high-priority packets. Second thread selects packets with high weight and sends (resends in fact) them. Resending happens with limited speed, and thus it shapes traffic.

Packet weight is assigned in "modules", there is 4 out of box.

- inhibit_big_flows - suppresses big flows. The more bytes transmitted between two IP addresses, the less weight of a packet in this flow.

- bymark - packet weight is set by iptables mark. See damper.conf for details and example.

- entropy - Shannon entropy calculated for each flow and used as weight. Flow identified by IP addresses, protocol number and source/destination ports in case of TCP or UDP. The more random is traffic (encrypted, compressed), the less weight is set to packet.

- random - generates a random weight (as in classic RED shaping algorithm)

To enable or disable module edit `modules.conf.c` file

Weight from each module multiplied by module coefficient and summarized to get the final value.

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

Make directory for statistics, if you plan to use it. And run shaper

```sh
# mkdir -p /var/lib/damper
# ./damper damper.conf &
```

### Running on router (shaping forwarded traffic)

Shaping outgoing TCP traffic ("upload" as seen from user, egress)

```sh
# iptables -t raw -A OUTPUT -m mark --mark 88 -j ACCEPT
# iptables -t raw -A PREROUTING -i eth0 -p tcp -j NFQUEUE --queue-num 3
```
where `eth0` - internal router interface

Shaping incoming TCP traffic destined to our network 192.168.0.0/24 ("download" as seen from user, ingress)

```sh
# iptables -t raw -A OUTPUT -m mark --mark 88 -j NOTRACK
# iptables -t mangle -A FORWARD -d 192.168.0.0/24 -o eth0 -p tcp -j NFQUEUE --queue-num 3
```

Be very careful with these rules, you may lose you router network connectivity

### Statistics

damper comes with web-based statistics viewer, demo is available (or unavailable sometimes) here: http://damper.xenoeye.com

Chart displayed using SCGI module. It can be integrated with Apache, Nginx, lighthttp or any web-server which support SCGI interface

To compile module:

(libpng required)

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

For Apache add this string to &lt;VirtualHost&gt;:

```
SCGIMount /damper-img 127.0.0.1:9001
```
