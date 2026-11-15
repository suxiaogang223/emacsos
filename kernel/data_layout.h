#ifndef EMACSOS_DATA_LAYOUT_H
#define EMACSOS_DATA_LAYOUT_H

#define EMACSOS_HOME "/home/user"
#define EMACSOS_DEFAULT_PASSWD "root:x:0:0:emacsOS:" EMACSOS_HOME ":\n"
#define EMACSOS_DEFAULT_GROUP "root:x:0:\n"

/* Root anchors are fixed; ordinary descendants remain application-managed. */
#define EMACSOS_DATA_ANCHORS(X) \
    X("etc") \
    X("home") \
    X("tmp") \
    X("var")

/* Parent directories precede children; this also seeds read-only recovery. */
#define EMACSOS_DATA_DIRECTORIES(X) \
    X("/etc", 0755) \
    X("/home", 0755) \
    X("/tmp", 01777) \
    X("/var", 0755) \
    X("/etc/emacsos", 0755) \
    X(EMACSOS_HOME, 0755) \
    X(EMACSOS_HOME "/projects", 0755) \
    X("/var/lib", 0755) \
    X("/var/lib/emacsos", 0755) \
    X("/var/log", 0755) \
    X("/var/cache", 0755)

#endif
