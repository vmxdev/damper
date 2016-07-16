# damper
Linux userspace traffic shaper

Utility for shaping network traffic. `damper` uses NFQUEUE mechanism for capture traffic and raw sockets for re-injecting. All processing occurs in user space as opposed to traditional kernel space Linux traffic shaping.

### Compiling

```sh
$ cc -Wall -pedantic damper.c modules.conf.c -o damper -lnetfilter_queue -pthread -lrt
```

### Running

For shaping outgoing locally generated TCP traffic

First allow reinjected traffic. It prevents the same network packet to be passed to shaper again. Reinjected packets are marked in shaper ("mark" directive in damper.conf).

```sh
# iptables -t raw -A OUTPUT -m mark --mark 88 -j ACCEPT
```

Shaper can change DSCP value in packet ("dscp" directive), so passing rule for reinjected traffic can be

```sh
# iptables -t raw -A OUTPUT -m dscp --dscp 2 -j ACCEPT
```

All locally-generated TCP traffic will be sent to shaper (which is listening on queue number 3, "queue 3" in damper.conf)

```sh
# iptables -t raw -A OUTPUT -p tcp -j NFQUEUE --queue-num 3
```

Matching packets will be dropped and generated in another order (based on shaper packets priority) on interface from damper.conf, "iface eth0" for example. In case of overlimit packets with lowest priority will not be sent (completely dropped).
