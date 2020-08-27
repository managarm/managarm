# mbus

> This part of the Handbook is Work-In-Progress.

mbus is the managarm message bus system. mbus is used by servers to publish objects, which other servers can discover. If a server finds an object of interest, it can then use mbus to connect to that server. Further communication happens over the more common hel streams documented in the [hel](../hel/index.md) folder of this handbook.

The code for mbus can be found at [https://github.com/managarm/managarm/tree/master/mbus](https://github.com/managarm/managarm/tree/master/mbus).