# damper
Linux userspace traffic shaper

Utility for shaping network traffic. `damper` uses NFQUEUE mechanism for traffic shaping. All processing occurs in user space as opposed to traditional kernel space Linux traffic shaping.

### Compiling

(netfilter-queue library required)

```sh
$ cc -Wall -pedantic damper.c modules.conf.c -o damper -lnetfilter_queue -pthread -lrt -lm
```

### Shaping and modules

`damper` works approximately in this way: at startup two threads are created. First thread captures network packets (via NFQUEUE), calculate "weight" (or priority) for each one and put it in priority queue. Wheh queue is full, packets with low priority replaced with high-priority ones. Second thread selects packets with high weight and sends (notify kernel to send in fact) them. Sending happens with limited speed (which is set in config file), and thus it shapes traffic.

Packet weight is assigned in "modules", there is 4 out of box.

- inhibit_big_flows - suppresses big flows. The more bytes transmitted between two IP addresses, the less weight of a packet in this flow.

- bymark - packet weight is set by iptables mark. See `damper.conf` for details and example.

- entropy - Shannon entropy calculated for each flow and used as weight. Flow identified by IP addresses, protocol number and source/destination ports in case of TCP or UDP. The more random is traffic (encrypted, compressed or multimedia traffic gets higher entropy values), the less weight is set to packet.

- random - generates a random weight (when this module is used alone, we get the classic RED shaping algorithm)

To enable or disable modules edit `modules.conf.c` file

Weight from each module multiplied by module coefficient and summarized to get the final value.

### Running on local box

For shaping outgoing locally generated TCP traffic add this rule to your iptables:

```sh
# iptables -t raw -A OUTPUT -p tcp -j NFQUEUE --queue-num 3 --queue-bypass
```

And here is rules for shaping incoming TCP traffic on interface eth0.

```sh
# iptables -t raw -A PREROUTING -i eth0 -p tcp -j NFQUEUE --queue-num 3 --queue-bypass
```

Make directory for statistics, if you plan to use it. And run shaper

```sh
# mkdir -p /var/lib/damper
# ./damper damper.conf &
```

### Running on router (shaping forwarded traffic)

Shaping outgoing TCP traffic ("upload" as seen from user, egress)

```sh
# iptables -t raw -A PREROUTING -i eth0 -p tcp -j NFQUEUE --queue-num 3 --queue-bypass
```
where `eth0` - internal router interface

`--queue-bypass` option change the behavior of iptables when no userspace application is connected to the queue. Instead of dropping packets are passed throwgh

Shaping incoming TCP traffic destined to our network 192.168.0.0/24 ("download" as seen from user, ingress)

```sh
# iptables -t mangle -A FORWARD -d 192.168.0.0/24 -o eth0 -p tcp -j NFQUEUE --queue-num 3 --queue-bypass
```

Be very careful with these rules, you may lose you router network connectivity

### Statistics

damper comes with web-based statistics viewer

By default it keeps statistics for last 31 days. Number of days to hold statistics can be altered by changing `keepstat` key in config.

You can zoom or pan chart by mouse, double-click shows stats for all the observation period

Chart displayed using SCGI module. It can be integrated with Apache, Nginx, lighthttp or any web-server which support SCGI interface

To compile module:

(libpng required)

```sh
$ cd stat
$ cc -O2 -Wall -pedantic damper_img.c image.c stats.c -o damper_img -lpng -pthread
```

Copy content of `damper/stat/html/` directory to server root directory

Now you can run it and configure http-server

```sh
# ./damper_img 127.0.0.1:9001 /var/lib/damper/ www-data &
```

damper_img will drop privileges to `www-data` and chroots to `/var/lib/damper/`

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
