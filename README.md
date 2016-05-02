# damper
Linux userspace traffic shaper

Utility for shaping network traffic. `damper` uses NFQUEUE mechanism for capture traffic and raw sockets for re-injecting. All processing occurs in user space as opposed to traditional kernel space Linux traffic shaping.

### Compiling

```sh
$ cc -Wall -pedantic damper.c modules.conf.c -o damper -lnetfilter_queue -pthread -lrt
```
