/*
    FUSE: Filesystem in Userspace
    Copyright (C) 2001-2006  Miklos Szeredi <miklos@szeredi.hu>

    This program can be distributed under the terms of the GNU LGPL.
    See the file COPYING.LIB
*/


/* For pthread_rwlock_t */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "xlator.h"
#include "transport.h"

#include "fuse-internals.h"


static struct node *get_node_nocheck(struct fuse *f, fuse_ino_t nodeid)
{
    size_t hash = nodeid % f->id_table_size;
    struct node *node;

    for (node = f->id_table[hash]; node != NULL; node = node->id_next)
        if (node->nodeid == nodeid)
            return node;

    return NULL;
}

static struct node *get_node(struct fuse *f, fuse_ino_t nodeid)
{
    struct node *node = get_node_nocheck(f, nodeid);
    if (!node) {
        fprintf(stderr, "fuse internal error: node %llu not found\n",
                (unsigned long long) nodeid);
        abort();
    }
    return node;
}

static void free_node(struct node *node)
{
    free(node->name);
    free(node);
}

static void unhash_id(struct fuse *f, struct node *node)
{
    size_t hash = node->nodeid % f->id_table_size;
    struct node **nodep = &f->id_table[hash];

    for (; *nodep != NULL; nodep = &(*nodep)->id_next)
        if (*nodep == node) {
            *nodep = node->id_next;
            return;
        }
}

static void hash_id(struct fuse *f, struct node *node)
{
    size_t hash = node->nodeid % f->id_table_size;
    node->id_next = f->id_table[hash];
    f->id_table[hash] = node;
}

static unsigned int name_hash(struct fuse *f, fuse_ino_t parent, const char *name)
{
    unsigned int hash = *name;

    if (hash)
        for (name += 1; *name != '\0'; name++)
            hash = (hash << 5) - hash + *name;

    return (hash + parent) % f->name_table_size;
}

static void unref_node(struct fuse *f, struct node *node);

static void unhash_name(struct fuse *f, struct node *node)
{
    if (node->name) {
        size_t hash = name_hash(f, node->parent, node->name);
        struct node **nodep = &f->name_table[hash];

        for (; *nodep != NULL; nodep = &(*nodep)->name_next)
            if (*nodep == node) {
                *nodep = node->name_next;
                node->name_next = NULL;
                unref_node(f, get_node(f, node->parent));
                free(node->name);
                node->name = NULL;
                node->parent = 0;
                return;
            }
        fprintf(stderr, "fuse internal error: unable to unhash node: %llu\n",
                (unsigned long long) node->nodeid);
        abort();
    }
}

static int hash_name(struct fuse *f, struct node *node, fuse_ino_t parent,
                     const char *name)
{
    size_t hash = name_hash(f, parent, name);
    node->name = strdup(name);
    if (node->name == NULL)
        return -1;

    get_node(f, parent)->refctr ++;
    node->parent = parent;
    node->name_next = f->name_table[hash];
    f->name_table[hash] = node;
    return 0;
}

static void delete_node(struct fuse *f, struct node *node)
{
    if (f->conf.debug) {
        printf("delete: %llu\n", (unsigned long long) node->nodeid);
        fflush(stdout);
    }
    assert(!node->name);
    unhash_id(f, node);
    free_node(node);
}

static void unref_node(struct fuse *f, struct node *node)
{
    assert(node->refctr > 0);
    node->refctr --;
    if (!node->refctr)
        delete_node(f, node);
}

static fuse_ino_t next_id(struct fuse *f)
{
    do {
        f->ctr++;
        if (!f->ctr)
            f->generation ++;
    } while (f->ctr == 0 || get_node_nocheck(f, f->ctr) != NULL);
    return f->ctr;
}

static struct node *lookup_node(struct fuse *f, fuse_ino_t parent,
                                const char *name)
{
    size_t hash = name_hash(f, parent, name);
    struct node *node;

    for (node = f->name_table[hash]; node != NULL; node = node->name_next)
        if (node->parent == parent && strcmp(node->name, name) == 0)
            return node;

    return NULL;
}

static struct node *find_node(struct fuse *f, fuse_ino_t parent,
                              const char *name)
{
    struct node *node;

    pthread_mutex_lock(&f->lock);
    node = lookup_node(f, parent, name);
    if (node == NULL) {
        node = (struct node *) calloc(1, sizeof(struct node));
        if (node == NULL)
            goto out_err;

        node->refctr = 1;
        node->nodeid = next_id(f);
        node->open_count = 0;
        node->is_hidden = 0;
        node->generation = f->generation;
        if (hash_name(f, node, parent, name) == -1) {
            free(node);
            node = NULL;
            goto out_err;
        }
        hash_id(f, node);
    }
    node->nlookup ++;
 out_err:
    pthread_mutex_unlock(&f->lock);
    return node;
}

static char *add_name(char *buf, char *s, const char *name)
{
    size_t len = strlen(name);
    s -= len;
    if (s <= buf) {
        fprintf(stderr, "fuse: path too long: ...%s\n", s + len);
        return NULL;
    }
    strncpy(s, name, len);
    s--;
    *s = '/';

    return s;
}

static char *get_path_name(struct fuse *f, fuse_ino_t nodeid, const char *name)
{
    char buf[FUSE_MAX_PATH];
    char *s = buf + FUSE_MAX_PATH - 1;
    struct node *node;

    *s = '\0';

    if (name != NULL) {
        s = add_name(buf, s, name);
        if (s == NULL)
            return NULL;
    }

    pthread_mutex_lock(&f->lock);
    for (node = get_node(f, nodeid); node && node->nodeid != FUSE_ROOT_ID;
         node = get_node(f, node->parent)) {
        if (node->name == NULL) {
            s = NULL;
            break;
        }

        s = add_name(buf, s, node->name);
        if (s == NULL)
            break;
    }
    pthread_mutex_unlock(&f->lock);

    if (node == NULL || s == NULL)
        return NULL;
    else if (*s == '\0')
        return strdup("/");
    else
        return strdup(s);
}

static char *get_path(struct fuse *f, fuse_ino_t nodeid)
{
    return get_path_name(f, nodeid, NULL);
}

static void forget_node(struct fuse *f, fuse_ino_t nodeid, uint64_t nlookup)
{
    struct node *node;
    if (nodeid == FUSE_ROOT_ID)
        return;
    pthread_mutex_lock(&f->lock);
    node = get_node(f, nodeid);
    assert(node->nlookup >= nlookup);
    node->nlookup -= nlookup;
    if (!node->nlookup) {
        unhash_name(f, node);
        unref_node(f, node);
    }
    pthread_mutex_unlock(&f->lock);
}

static void remove_node(struct fuse *f, fuse_ino_t dir, const char *name)
{
    struct node *node;

    pthread_mutex_lock(&f->lock);
    node = lookup_node(f, dir, name);
    if (node != NULL)
        unhash_name(f, node);
    pthread_mutex_unlock(&f->lock);
}

static int rename_node(struct fuse *f, fuse_ino_t olddir, const char *oldname,
                        fuse_ino_t newdir, const char *newname, int hide)
{
    struct node *node;
    struct node *newnode;
    int err = 0;

    pthread_mutex_lock(&f->lock);
    node  = lookup_node(f, olddir, oldname);
    newnode  = lookup_node(f, newdir, newname);
    if (node == NULL)
        goto out;

    if (newnode != NULL) {
        if (hide) {
            fprintf(stderr, "fuse: hidden file got created during hiding\n");
            err = -EBUSY;
            goto out;
        }
        unhash_name(f, newnode);
    }

    unhash_name(f, node);
    if (hash_name(f, node, newdir, newname) == -1) {
        err = -ENOMEM;
        goto out;
    }

    if (hide)
        node->is_hidden = 1;

 out:
    pthread_mutex_unlock(&f->lock);
    return err;
}

static void set_stat(struct fuse *f, fuse_ino_t nodeid, struct stat *stbuf)
{
    if (!f->conf.use_ino)
        stbuf->st_ino = nodeid;
    if (f->conf.set_mode)
        stbuf->st_mode = (stbuf->st_mode & S_IFMT) | (0777 & ~f->conf.umask);
    if (f->conf.set_uid)
        stbuf->st_uid = f->conf.uid;
    if (f->conf.set_gid)
        stbuf->st_gid = f->conf.gid;
}

static int is_open(struct fuse *f, fuse_ino_t dir, const char *name)
{
    struct node *node;
    int isopen = 0;
    pthread_mutex_lock(&f->lock);
    node = lookup_node(f, dir, name);
    if (node && node->open_count > 0)
        isopen = 1;
    pthread_mutex_unlock(&f->lock);
    return isopen;
}

static char *hidden_name(struct fuse *f, fuse_ino_t dir, const char *oldname,
                        char *newname, size_t bufsize)
{
    struct stat buf;
    struct node *node;
    struct node *newnode;
    char *newpath;
    int res;
    int failctr = 10;

    if (!f->op.getattr)
        return NULL;

    do {
        pthread_mutex_lock(&f->lock);
        node = lookup_node(f, dir, oldname);
        if (node == NULL) {
            pthread_mutex_unlock(&f->lock);
            return NULL;
        }
        do {
            f->hidectr ++;
            snprintf(newname, bufsize, ".fuse_hidden%08x%08x",
                     (unsigned int) node->nodeid, f->hidectr);
            newnode = lookup_node(f, dir, newname);
        } while(newnode);
        pthread_mutex_unlock(&f->lock);

        newpath = get_path_name(f, dir, newname);
        if (!newpath)
            break;

        res = f->op.getattr(newpath, &buf);
        if (res != 0)
            break;
        free(newpath);
        newpath = NULL;
    } while(--failctr);

    return newpath;
}

static int hide_node(struct fuse *f, const char *oldpath, fuse_ino_t dir,
                     const char *oldname)
{
    char newname[64];
    char *newpath;
    int err = -EBUSY;

    if (f->op.rename && f->op.unlink) {
        newpath = hidden_name(f, dir, oldname, newname, sizeof(newname));
        if (newpath) {
            int res = f->op.rename(oldpath, newpath);
            if (res == 0)
                err = rename_node(f, dir, oldname, dir, newname, 1);
            free(newpath);
        }
    }
    return err;
}

static int lookup_path(struct fuse *f, fuse_ino_t nodeid, const char *name,
                       const char *path, struct fuse_entry_param *e,
                       struct fuse_file_info *fi)
{
    int res;

    memset(e, 0, sizeof(struct fuse_entry_param));
    if (fi && f->op.fgetattr)
        res = f->op.fgetattr(path, &e->attr, fi);
    else
        res = f->op.getattr(path, &e->attr);
    if (res == 0) {
        struct node *node;

        node = find_node(f, nodeid, name);
        if (node == NULL)
            res = -ENOMEM;
        else {
            e->ino = node->nodeid;
            e->generation = node->generation;
            e->entry_timeout = f->conf.entry_timeout;
            e->attr_timeout = f->conf.attr_timeout;
            set_stat(f, e->ino, &e->attr);
            if (f->conf.debug) {
                printf("   NODEID: %lu\n", (unsigned long) e->ino);
                fflush(stdout);
            }
        }
    }
    return res;
}

static struct fuse *req_fuse(fuse_req_t req)
{
    return (struct fuse *) fuse_req_userdata(req);
}

static struct fuse *req_fuse_prepare(fuse_req_t req)
{
    struct fuse_context *c = fuse_get_context();
    const struct fuse_ctx *ctx = fuse_req_ctx(req);
    c->fuse = req_fuse(req);
    c->uid = ctx->uid;
    c->gid = ctx->gid;
    c->pid = ctx->pid;
    c->private_data = c->fuse->user_data;

    return c->fuse;
}

static inline void reply_err(fuse_req_t req, int err)
{
    /* fuse_reply_err() uses non-negated errno values */
    fuse_reply_err(req, -err);
}

static void reply_entry(fuse_req_t req, const struct fuse_entry_param *e,
                        int err)
{
    if (!err) {
        struct fuse *f = req_fuse(req);
        if (fuse_reply_entry(req, e) == -ENOENT)
            forget_node(f, e->ino, 1);
    } else
        reply_err(req, err);
}

static void fuse_data_init(void *data)
{
    struct fuse *f = (struct fuse *) data;
    struct fuse_context *c = fuse_get_context();

    memset(c, 0, sizeof(*c));
    c->fuse = f;

    if (f->op.init)
        f->user_data = f->op.init();
}

static void fuse_data_destroy(void *data)
{
    struct fuse *f = (struct fuse *) data;
    struct fuse_context *c = fuse_get_context();

    memset(c, 0, sizeof(*c));
    c->fuse = f;
    c->private_data = f->user_data;

    if (f->op.destroy)
        f->op.destroy(f->user_data);
}

static void fuse_lookup(fuse_req_t req, fuse_ino_t parent, const char *name)
{
    struct fuse *f = req_fuse_prepare(req);
    struct fuse_entry_param e;
    char *path;
    int err;

    err = -ENOENT;
    pthread_rwlock_rdlock(&f->tree_lock);
    path = get_path_name(f, parent, name);
    if (path != NULL) {
        if (f->conf.debug) {
            printf("LOOKUP %s\n", path);
            fflush(stdout);
        }
        err = -ENOSYS;
        if (f->op.getattr) {
            err = lookup_path(f, parent, name, path, &e, NULL);
            if (err == -ENOENT && f->conf.negative_timeout != 0.0) {
                e.ino = 0;
                e.entry_timeout = f->conf.negative_timeout;
                err = 0;
            }
        }
        free(path);
    }
    pthread_rwlock_unlock(&f->tree_lock);
    reply_entry(req, &e, err);
}

static void fuse_forget(fuse_req_t req, fuse_ino_t ino, unsigned long nlookup)
{
    struct fuse *f = req_fuse(req);
    if (f->conf.debug) {
        printf("FORGET %llu/%lu\n", (unsigned long long) ino, nlookup);
        fflush(stdout);
    }
    forget_node(f, ino, nlookup);
    fuse_reply_none(req);
}

static void fuse_getattr(fuse_req_t req, fuse_ino_t ino,
                         struct fuse_file_info *fi)
{
    struct fuse *f = req_fuse_prepare(req);
    struct stat buf;
    char *path;
    int err;

    (void) fi;

    err = -ENOENT;
    pthread_rwlock_rdlock(&f->tree_lock);
    path = get_path(f, ino);
    if (path != NULL) {
        err = -ENOSYS;
        if (f->op.getattr)
            err = f->op.getattr(path, &buf);
        free(path);
    }
    pthread_rwlock_unlock(&f->tree_lock);
    if (!err) {
        set_stat(f, ino, &buf);
        fuse_reply_attr(req, &buf, f->conf.attr_timeout);
    } else
        reply_err(req, err);
}

static int do_chmod(struct fuse *f, const char *path, struct stat *attr)
{
    int err;

    err = -ENOSYS;
    if (f->op.chmod)
        err = f->op.chmod(path, attr->st_mode);

    return err;
}

static int do_chown(struct fuse *f, const char *path, struct stat *attr,
                    int valid)
{
    int err;
    uid_t uid = (valid & FUSE_SET_ATTR_UID) ? attr->st_uid : (uid_t) -1;
    gid_t gid = (valid & FUSE_SET_ATTR_GID) ? attr->st_gid : (gid_t) -1;

    err = -ENOSYS;
    if (f->op.chown)
        err = f->op.chown(path, uid, gid);

    return err;
}

static int do_truncate(struct fuse *f, const char *path, struct stat *attr,
                       struct fuse_file_info *fi)
{
    int err;

    err = -ENOSYS;
    if (fi && f->op.ftruncate)
        err = f->op.ftruncate(path, attr->st_size, fi);
    else if (f->op.truncate)
        err = f->op.truncate(path, attr->st_size);

    return err;
}

static int do_utime(struct fuse *f, const char *path, struct stat *attr)
{
    int err;
    struct utimbuf buf;
    buf.actime = attr->st_atime;
    buf.modtime = attr->st_mtime;
    err = -ENOSYS;
    if (f->op.utime)
        err = f->op.utime(path, &buf);

    return err;
}

static void fuse_setattr(fuse_req_t req, fuse_ino_t ino, struct stat *attr,
                         int valid, struct fuse_file_info *fi)
{
    struct fuse *f = req_fuse_prepare(req);
    struct stat buf;
    char *path;
    int err;

    err = -ENOENT;
    pthread_rwlock_rdlock(&f->tree_lock);
    path = get_path(f, ino);
    if (path != NULL) {
        err = -ENOSYS;
        if (f->op.getattr) {
            err = 0;
            if (!err && (valid & FUSE_SET_ATTR_MODE))
                err = do_chmod(f, path, attr);
            if (!err && (valid & (FUSE_SET_ATTR_UID | FUSE_SET_ATTR_GID)))
                err = do_chown(f, path, attr, valid);
            if (!err && (valid & FUSE_SET_ATTR_SIZE))
                err = do_truncate(f, path, attr, fi);
            if (!err && (valid & (FUSE_SET_ATTR_ATIME | FUSE_SET_ATTR_MTIME)) == (FUSE_SET_ATTR_ATIME | FUSE_SET_ATTR_MTIME))
                err = do_utime(f, path, attr);
            if (!err)
                err = f->op.getattr(path, &buf);
        }
        free(path);
    }
    pthread_rwlock_unlock(&f->tree_lock);
    if (!err) {
        set_stat(f, ino, &buf);
        fuse_reply_attr(req, &buf, f->conf.attr_timeout);
    } else
        reply_err(req, err);
}

static void fuse_access(fuse_req_t req, fuse_ino_t ino, int mask)
{
    struct fuse *f = req_fuse_prepare(req);
    char *path;
    int err;

    err = -ENOENT;
    pthread_rwlock_rdlock(&f->tree_lock);
    path = get_path(f, ino);
    if (path != NULL) {
        if (f->conf.debug) {
            printf("ACCESS %s 0%o\n", path, mask);
            fflush(stdout);
        }
        err = -ENOSYS;
        if (f->op.access)
            err = f->op.access(path, mask);
        free(path);
    }
    pthread_rwlock_unlock(&f->tree_lock);
    reply_err(req, err);
}

static void fuse_readlink(fuse_req_t req, fuse_ino_t ino)
{
    struct fuse *f = req_fuse_prepare(req);
    char linkname[PATH_MAX + 1];
    char *path;
    int err;

    err = -ENOENT;
    pthread_rwlock_rdlock(&f->tree_lock);
    path = get_path(f, ino);
    if (path != NULL) {
        err = -ENOSYS;
        if (f->op.readlink)
            err = f->op.readlink(path, linkname, sizeof(linkname));
        free(path);
    }
    pthread_rwlock_unlock(&f->tree_lock);
    if (!err) {
        linkname[PATH_MAX] = '\0';
        fuse_reply_readlink(req, linkname);
    } else
        reply_err(req, err);
}

static void fuse_mknod(fuse_req_t req, fuse_ino_t parent, const char *name,
                       mode_t mode, dev_t rdev)
{
    struct fuse *f = req_fuse_prepare(req);
    struct fuse_entry_param e;
    char *path;
    int err;

    err = -ENOENT;
    pthread_rwlock_rdlock(&f->tree_lock);
    path = get_path_name(f, parent, name);
    if (path != NULL) {
        if (f->conf.debug) {
            printf("MKNOD %s\n", path);
            fflush(stdout);
        }
        err = -ENOSYS;
        if (S_ISREG(mode) && f->op.create && f->op.getattr) {
            struct fuse_file_info fi;

            memset(&fi, 0, sizeof(fi));
            fi.flags = O_CREAT | O_EXCL | O_WRONLY;
            err = f->op.create(path, mode, &fi);
            if (!err) {
                err = lookup_path(f, parent, name, path, &e, &fi);
                if (f->op.release)
                    f->op.release(path, &fi);
            }
        } else if (f->op.mknod && f->op.getattr) {
            err = f->op.mknod(path, mode, rdev);
            if (!err)
                err = lookup_path(f, parent, name, path, &e, NULL);
        }
        free(path);
    }
    pthread_rwlock_unlock(&f->tree_lock);
    reply_entry(req, &e, err);
}

static void fuse_mkdir(fuse_req_t req, fuse_ino_t parent, const char *name,
                       mode_t mode)
{
    struct fuse *f = req_fuse_prepare(req);
    struct fuse_entry_param e;
    char *path;
    int err;

    err = -ENOENT;
    pthread_rwlock_rdlock(&f->tree_lock);
    path = get_path_name(f, parent, name);
    if (path != NULL) {
        if (f->conf.debug) {
            printf("MKDIR %s\n", path);
            fflush(stdout);
        }
        err = -ENOSYS;
        if (f->op.mkdir && f->op.getattr) {
            err = f->op.mkdir(path, mode);
            if (!err)
                err = lookup_path(f, parent, name, path, &e, NULL);
        }
        free(path);
    }
    pthread_rwlock_unlock(&f->tree_lock);
    reply_entry(req, &e, err);
}

static void fuse_unlink(fuse_req_t req, fuse_ino_t parent, const char *name)
{
    struct fuse *f = req_fuse_prepare(req);
    char *path;
    int err;

    err = -ENOENT;
    pthread_rwlock_wrlock(&f->tree_lock);
    path = get_path_name(f, parent, name);
    if (path != NULL) {
        if (f->conf.debug) {
            printf("UNLINK %s\n", path);
            fflush(stdout);
        }
        err = -ENOSYS;
        if (f->op.unlink) {
            if (!f->conf.hard_remove && is_open(f, parent, name))
                err = hide_node(f, path, parent, name);
            else {
                err = f->op.unlink(path);
                if (!err)
                    remove_node(f, parent, name);
            }
        }
        free(path);
    }
    pthread_rwlock_unlock(&f->tree_lock);
    reply_err(req, err);
}

static void fuse_rmdir(fuse_req_t req, fuse_ino_t parent, const char *name)
{
    struct fuse *f = req_fuse_prepare(req);
    char *path;
    int err;

    err = -ENOENT;
    pthread_rwlock_wrlock(&f->tree_lock);
    path = get_path_name(f, parent, name);
    if (path != NULL) {
        if (f->conf.debug) {
            printf("RMDIR %s\n", path);
            fflush(stdout);
        }
        err = -ENOSYS;
        if (f->op.rmdir) {
            err = f->op.rmdir(path);
            if (!err)
                remove_node(f, parent, name);
        }
        free(path);
    }
    pthread_rwlock_unlock(&f->tree_lock);
    reply_err(req, err);
}

static void fuse_symlink(fuse_req_t req, const char *linkname,
                         fuse_ino_t parent, const char *name)
{
    struct fuse *f = req_fuse_prepare(req);
    struct fuse_entry_param e;
    char *path;
    int err;

    err = -ENOENT;
    pthread_rwlock_rdlock(&f->tree_lock);
    path = get_path_name(f, parent, name);
    if (path != NULL) {
        if (f->conf.debug) {
            printf("SYMLINK %s\n", path);
            fflush(stdout);
        }
        err = -ENOSYS;
        if (f->op.symlink && f->op.getattr) {
            err = f->op.symlink(linkname, path);
            if (!err)
                err = lookup_path(f, parent, name, path, &e, NULL);
        }
        free(path);
    }
    pthread_rwlock_unlock(&f->tree_lock);
    reply_entry(req, &e, err);
}

static void fuse_rename(fuse_req_t req, fuse_ino_t olddir, const char *oldname,
                        fuse_ino_t newdir, const char *newname)
{
    struct fuse *f = req_fuse_prepare(req);
    char *oldpath;
    char *newpath;
    int err;

    err = -ENOENT;
    pthread_rwlock_wrlock(&f->tree_lock);
    oldpath = get_path_name(f, olddir, oldname);
    if (oldpath != NULL) {
        newpath = get_path_name(f, newdir, newname);
        if (newpath != NULL) {
            if (f->conf.debug) {
                printf("RENAME %s -> %s\n", oldpath, newpath);
                fflush(stdout);
            }
            err = -ENOSYS;
            if (f->op.rename) {
                err = 0;
                if (!f->conf.hard_remove &&
                    is_open(f, newdir, newname))
                    err = hide_node(f, newpath, newdir, newname);
                if (!err) {
                    err = f->op.rename(oldpath, newpath);
                    if (!err)
                        err = rename_node(f, olddir, oldname, newdir, newname, 0);
                }
            }
            free(newpath);
        }
        free(oldpath);
    }
    pthread_rwlock_unlock(&f->tree_lock);
    reply_err(req, err);
}

static void fuse_link(fuse_req_t req, fuse_ino_t ino, fuse_ino_t newparent,
                      const char *newname)
{
    struct fuse *f = req_fuse_prepare(req);
    struct fuse_entry_param e;
    char *oldpath;
    char *newpath;
    int err;

    err = -ENOENT;
    pthread_rwlock_rdlock(&f->tree_lock);
    oldpath = get_path(f, ino);
    if (oldpath != NULL) {
        newpath =  get_path_name(f, newparent, newname);
        if (newpath != NULL) {
            if (f->conf.debug) {
                printf("LINK %s\n", newpath);
                fflush(stdout);
            }
            err = -ENOSYS;
            if (f->op.link && f->op.getattr) {
                err = f->op.link(oldpath, newpath);
                if (!err)
                    err = lookup_path(f, newparent, newname, newpath, &e, NULL);
            }
            free(newpath);
        }
        free(oldpath);
    }
    pthread_rwlock_unlock(&f->tree_lock);
    reply_entry(req, &e, err);
}

static void fuse_create(fuse_req_t req, fuse_ino_t parent, const char *name,
                        mode_t mode, struct fuse_file_info *fi)
{
    struct fuse *f = req_fuse_prepare(req);
    struct fuse_entry_param e;
    char *path;
    int err;

    err = -ENOENT;
    pthread_rwlock_rdlock(&f->tree_lock);
    path = get_path_name(f, parent, name);
    if (path != NULL) {
        err = -ENOSYS;
        if (f->op.create && f->op.getattr) {
            err = f->op.create(path, mode, fi);
            if (!err) {
                if (f->conf.debug) {
                    printf("CREATE[%llu] flags: 0x%x %s\n",
                           (unsigned long long) fi->fh, fi->flags, path);
                    fflush(stdout);
                }
                err = lookup_path(f, parent, name, path, &e, fi);
                if (err) {
                    if (f->op.release)
                        f->op.release(path, fi);
                } else if (!S_ISREG(e.attr.st_mode)) {
                    err = -EIO;
                    if (f->op.release)
                        f->op.release(path, fi);
                    forget_node(f, e.ino, 1);
                }
            }
        }
    }

    if (!err) {
        if (f->conf.direct_io)
            fi->direct_io = 1;
        if (f->conf.kernel_cache)
            fi->keep_cache = 1;

        pthread_mutex_lock(&f->lock);
        if (fuse_reply_create(req, &e, fi) == -ENOENT) {
            /* The open syscall was interrupted, so it must be cancelled */
            if(f->op.release)
                f->op.release(path, fi);
            forget_node(f, e.ino, 1);
        } else {
            struct node *node = get_node(f, e.ino);
            node->open_count ++;
        }
        pthread_mutex_unlock(&f->lock);
    } else
        reply_err(req, err);

    if (path)
        free(path);
    pthread_rwlock_unlock(&f->tree_lock);
}

static void fuse_open(fuse_req_t req, fuse_ino_t ino,
                      struct fuse_file_info *fi)
{
    struct fuse *f = req_fuse_prepare(req);
    char *path = NULL;
    int err = 0;

    pthread_rwlock_rdlock(&f->tree_lock);
    if (f->op.open) {
        err = -ENOENT;
        path = get_path(f, ino);
        if (path != NULL)
            err = f->op.open(path, fi);
    }
    if (!err) {
        if (f->conf.debug) {
            printf("OPEN[%llu] flags: 0x%x\n", (unsigned long long) fi->fh,
                   fi->flags);
            fflush(stdout);
        }

        if (f->conf.direct_io)
            fi->direct_io = 1;
        if (f->conf.kernel_cache)
            fi->keep_cache = 1;

        pthread_mutex_lock(&f->lock);
        if (fuse_reply_open(req, fi) == -ENOENT) {
            /* The open syscall was interrupted, so it must be cancelled */
            if(f->op.release && path != NULL)
                f->op.release (path, fi);
        } else {
            struct node *node = get_node(f, ino);
            node->open_count ++;
        }
        pthread_mutex_unlock(&f->lock);
    } else
        reply_err(req, err);

    if (path)
        free(path);
    pthread_rwlock_unlock(&f->tree_lock);
}

static void fuse_read(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off,
                      struct fuse_file_info *fi)
{
    struct fuse *f = req_fuse_prepare(req);
    char *path;
    char *buf;
    int res;

    buf = (char *) malloc(size);
    if (buf == NULL) {
        reply_err(req, -ENOMEM);
        return;
    }

    res = -ENOENT;
    pthread_rwlock_rdlock(&f->tree_lock);
    path = get_path(f, ino);
    if (path != NULL) {
        if (f->conf.debug) {
            printf("READ[%llu] %u bytes from %llu\n",
                   (unsigned long long) fi->fh, size, off);
            fflush(stdout);
        }

        res = -ENOSYS;
        if (f->op.read)
            res = f->op.read(path, buf, size, off, fi);
        free(path);
    }
    pthread_rwlock_unlock(&f->tree_lock);

    if (res >= 0) {
        if (f->conf.debug) {
            printf("   READ[%llu] %u bytes\n", (unsigned long long) fi->fh,
                   res);
            fflush(stdout);
        }
        if ((size_t) res > size)
            fprintf(stderr, "fuse: read too many bytes");
        fuse_reply_buf(req, buf, res);
    } else
        reply_err(req, res);

    free(buf);
}

static void fuse_write(fuse_req_t req, fuse_ino_t ino, const char *buf,
                       size_t size, off_t off, struct fuse_file_info *fi)
{
    struct fuse *f = req_fuse_prepare(req);
    char *path;
    int res;

    res = -ENOENT;
    pthread_rwlock_rdlock(&f->tree_lock);
    path = get_path(f, ino);
    if (path != NULL) {
        if (f->conf.debug) {
            printf("WRITE%s[%llu] %u bytes to %llu\n",
                   fi->writepage ? "PAGE" : "", (unsigned long long) fi->fh,
                   size, off);
            fflush(stdout);
        }

        res = -ENOSYS;
        if (f->op.write)
            res = f->op.write(path, buf, size, off, fi);
        free(path);
    }
    pthread_rwlock_unlock(&f->tree_lock);

    if (res >= 0) {
        if (f->conf.debug) {
            printf("   WRITE%s[%llu] %u bytes\n",
                   fi->writepage ? "PAGE" : "", (unsigned long long) fi->fh,
                   res);
            fflush(stdout);
        }
        if ((size_t) res > size)
            fprintf(stderr, "fuse: wrote too many bytes");
        fuse_reply_write(req, res);
    } else
        reply_err(req, res);
}

static void fuse_flush(fuse_req_t req, fuse_ino_t ino,
                       struct fuse_file_info *fi)
{
    struct fuse *f = req_fuse_prepare(req);
    char *path;
    int err;

    err = -ENOENT;
    pthread_rwlock_rdlock(&f->tree_lock);
    path = get_path(f, ino);
    if (path != NULL) {
        if (f->conf.debug) {
            printf("FLUSH[%llu]\n", (unsigned long long) fi->fh);
            fflush(stdout);
        }
        err = -ENOSYS;
        if (f->op.flush)
            err = f->op.flush(path, fi);
        free(path);
    }
    pthread_rwlock_unlock(&f->tree_lock);
    reply_err(req, err);
}

static void fuse_release(fuse_req_t req, fuse_ino_t ino,
                         struct fuse_file_info *fi)
{
    struct fuse *f = req_fuse_prepare(req);
    char *path;
    struct node *node;
    int unlink_hidden;

    pthread_rwlock_rdlock(&f->tree_lock);
    pthread_mutex_lock(&f->lock);
    node = get_node(f, ino);
    assert(node->open_count > 0);
    --node->open_count;
    unlink_hidden = (node->is_hidden && !node->open_count);
    pthread_mutex_unlock(&f->lock);

    path = get_path(f, ino);
    if (f->conf.debug) {
        printf("RELEASE[%llu] flags: 0x%x\n", (unsigned long long) fi->fh,
               fi->flags);
        fflush(stdout);
    }
    if (f->op.release)
        f->op.release(path, fi);

    if(unlink_hidden && path)
        f->op.unlink(path);

    if (path)
        free(path);
    pthread_rwlock_unlock(&f->tree_lock);

    reply_err(req, 0);
}

static void fuse_fsync(fuse_req_t req, fuse_ino_t ino, int datasync,
                       struct fuse_file_info *fi)
{
    struct fuse *f = req_fuse_prepare(req);
    char *path;
    int err;

    err = -ENOENT;
    pthread_rwlock_rdlock(&f->tree_lock);
    path = get_path(f, ino);
    if (path != NULL) {
        if (f->conf.debug) {
            printf("FSYNC[%llu]\n", (unsigned long long) fi->fh);
            fflush(stdout);
        }
        err = -ENOSYS;
        if (f->op.fsync)
            err = f->op.fsync(path, datasync, fi);
        free(path);
    }
    pthread_rwlock_unlock(&f->tree_lock);
    reply_err(req, err);
}

static struct fuse_dirhandle *get_dirhandle(const struct fuse_file_info *llfi,
                                            struct fuse_file_info *fi)
{
    struct fuse_dirhandle *dh = (struct fuse_dirhandle *) (uintptr_t) llfi->fh;
    memset(fi, 0, sizeof(struct fuse_file_info));
    fi->fh = dh->fh;
    fi->fh_old = dh->fh;
    return dh;
}

static void fuse_opendir(fuse_req_t req, fuse_ino_t ino,
                       struct fuse_file_info *llfi)
{
    struct fuse *f = req_fuse_prepare(req);
    struct fuse_dirhandle *dh;

    dh = (struct fuse_dirhandle *) malloc(sizeof(struct fuse_dirhandle));
    if (dh == NULL) {
        reply_err(req, -ENOMEM);
        return;
    }
    memset(dh, 0, sizeof(struct fuse_dirhandle));
    dh->fuse = f;
    dh->contents = NULL;
    dh->len = 0;
    dh->filled = 0;
    dh->nodeid = ino;
    pthread_mutex_init(&dh->lock, NULL);

    llfi->fh = (uintptr_t) dh;

    if (f->op.opendir) {
        struct fuse_file_info fi;
        char *path;
        int err;

        memset(&fi, 0, sizeof(fi));
        fi.flags = llfi->flags;

        err = -ENOENT;
        pthread_rwlock_rdlock(&f->tree_lock);
        path = get_path(f, ino);
        if (path != NULL) {
            err = f->op.opendir(path, &fi);
            dh->fh = fi.fh;
        }
        if (!err) {
            pthread_mutex_lock(&f->lock);
            if (fuse_reply_open(req, llfi) == -ENOENT) {
                /* The opendir syscall was interrupted, so it must be
                   cancelled */
                if(f->op.releasedir)
                    f->op.releasedir(path, &fi);
                pthread_mutex_destroy(&dh->lock);
                free(dh);
            }
            pthread_mutex_unlock(&f->lock);
        } else {
            reply_err(req, err);
            free(dh);
        }
        free(path);
        pthread_rwlock_unlock(&f->tree_lock);
    } else
        fuse_reply_open(req, llfi);
}

static int fill_dir_common(struct fuse_dirhandle *dh, const char *name,
                           const struct stat *statp, off_t off)
{
    struct stat stbuf;
    unsigned namelen = strlen(name);
    unsigned entsize;
    unsigned newlen;

    if (statp)
        stbuf = *statp;
    else {
        memset(&stbuf, 0, sizeof(stbuf));
        stbuf.st_ino = FUSE_UNKNOWN_INO;
    }

    if (!dh->fuse->conf.use_ino) {
        stbuf.st_ino = FUSE_UNKNOWN_INO;
        if (dh->fuse->conf.readdir_ino) {
            struct node *node;
            pthread_mutex_lock(&dh->fuse->lock);
            node = lookup_node(dh->fuse, dh->nodeid, name);
            if (node)
                stbuf.st_ino  = (ino_t) node->nodeid;
            pthread_mutex_unlock(&dh->fuse->lock);
        }
    }

    entsize = fuse_dirent_size(namelen);
    newlen = dh->len + entsize;

    if (off) {
        dh->filled = 0;
        if (newlen > dh->needlen)
            return 1;
    }

    if (newlen > dh->size) {
        char *newptr;

        if (!dh->size)
            dh->size = 1024;
        while (newlen > dh->size)
            dh->size *= 2;

        newptr = (char *) realloc(dh->contents, dh->size);
        if (!newptr) {
            dh->error = -ENOMEM;
            return 1;
        }
        dh->contents = newptr;
    }
    fuse_add_dirent(dh->contents + dh->len, name, &stbuf, off ? off : newlen);
    dh->len = newlen;
    return 0;
}

static int fill_dir(void *buf, const char *name, const struct stat *stbuf,
                    off_t off)
{
    return fill_dir_common((struct fuse_dirhandle *) buf, name, stbuf, off);
}

static int readdir_fill(struct fuse *f, fuse_ino_t ino, size_t size,
                        off_t off, struct fuse_dirhandle *dh,
                        struct fuse_file_info *fi)
{
    int err = -ENOENT;
    char *path;
    pthread_rwlock_rdlock(&f->tree_lock);
    path = get_path(f, ino);
    if (path != NULL) {
        dh->len = 0;
        dh->error = 0;
        dh->needlen = size;
        dh->filled = 1;
        err = -ENOSYS;
        if (f->op.readdir)
            err = f->op.readdir(path, dh, fill_dir, off, fi);
        if (!err)
            err = dh->error;
        if (err)
            dh->filled = 0;
        free(path);
    }
    pthread_rwlock_unlock(&f->tree_lock);
    return err;
}

static void fuse_readdir(fuse_req_t req, fuse_ino_t ino, size_t size,
                         off_t off, struct fuse_file_info *llfi)
{
    struct fuse *f = req_fuse_prepare(req);
    struct fuse_file_info fi;
    struct fuse_dirhandle *dh = get_dirhandle(llfi, &fi);

    pthread_mutex_lock(&dh->lock);
    /* According to SUS, directory contents need to be refreshed on
       rewinddir() */
    if (!off)
        dh->filled = 0;

    if (!dh->filled) {
        int err = readdir_fill(f, ino, size, off, dh, &fi);
        if (err) {
            reply_err(req, err);
            goto out;
        }
    }
    if (dh->filled) {
        if (off < dh->len) {
            if (off + size > dh->len)
                size = dh->len - off;
        } else
            size = 0;
    } else {
        size = dh->len;
        off = 0;
    }
    fuse_reply_buf(req, dh->contents + off, size);
 out:
    pthread_mutex_unlock(&dh->lock);
}

static void fuse_releasedir(fuse_req_t req, fuse_ino_t ino,
                            struct fuse_file_info *llfi)
{
    struct fuse *f = req_fuse_prepare(req);
    struct fuse_file_info fi;
    struct fuse_dirhandle *dh = get_dirhandle(llfi, &fi);
    if (f->op.releasedir) {
        char *path;

        pthread_rwlock_rdlock(&f->tree_lock);
        path = get_path(f, ino);
        f->op.releasedir(path ? path : "-", &fi);
        free(path);
        pthread_rwlock_unlock(&f->tree_lock);
    }
    pthread_mutex_lock(&dh->lock);
    pthread_mutex_unlock(&dh->lock);
    pthread_mutex_destroy(&dh->lock);
    free(dh->contents);
    free(dh);
    reply_err(req, 0);
}

static void fuse_fsyncdir(fuse_req_t req, fuse_ino_t ino, int datasync,
                          struct fuse_file_info *llfi)
{
    struct fuse *f = req_fuse_prepare(req);
    struct fuse_file_info fi;
    char *path;
    int err;

    get_dirhandle(llfi, &fi);

    err = -ENOENT;
    pthread_rwlock_rdlock(&f->tree_lock);
    path = get_path(f, ino);
    if (path != NULL) {
        err = -ENOSYS;
        if (f->op.fsyncdir)
            err = f->op.fsyncdir(path, datasync, &fi);
        free(path);
    }
    pthread_rwlock_unlock(&f->tree_lock);
    reply_err(req, err);
}

static int default_statfs(struct statvfs *buf)
{
    buf->f_namemax = 255;
    buf->f_bsize = 512;
    return 0;
}

static void fuse_statfs(fuse_req_t req)
{
    struct fuse *f = req_fuse_prepare(req);
    struct statvfs buf;
    int err;

    memset(&buf, 0, sizeof(buf));
    if (f->op.statfs) {
        err = f->op.statfs("/", &buf);
    } else
        err = default_statfs(&buf);

    if (!err)
        fuse_reply_statfs(req, &buf);
    else
        reply_err(req, err);
}

static void fuse_setxattr(fuse_req_t req, fuse_ino_t ino, const char *name,
                          const char *value, size_t size, int flags)
{
    struct fuse *f = req_fuse_prepare(req);
    char *path;
    int err;

    err = -ENOENT;
    pthread_rwlock_rdlock(&f->tree_lock);
    path = get_path(f, ino);
    if (path != NULL) {
        err = -ENOSYS;
        if (f->op.setxattr)
            err = f->op.setxattr(path, name, value, size, flags);
        free(path);
    }
    pthread_rwlock_unlock(&f->tree_lock);
    reply_err(req, err);
}

static int common_getxattr(struct fuse *f, fuse_ino_t ino, const char *name,
                           char *value, size_t size)
{
    int err;
    char *path;

    err = -ENOENT;
    pthread_rwlock_rdlock(&f->tree_lock);
    path = get_path(f, ino);
    if (path != NULL) {
        err = -ENOSYS;
        if (f->op.getxattr)
            err = f->op.getxattr(path, name, value, size);
        free(path);
    }
    pthread_rwlock_unlock(&f->tree_lock);
    return err;
}

static void fuse_getxattr(fuse_req_t req, fuse_ino_t ino, const char *name,
                        size_t size)
{
    struct fuse *f = req_fuse_prepare(req);
    int res;

    if (size) {
        char *value = (char *) malloc(size);
        if (value == NULL) {
            reply_err(req, -ENOMEM);
            return;
        }
        res = common_getxattr(f, ino, name, value, size);
        if (res > 0)
            fuse_reply_buf(req, value, res);
        else
            reply_err(req, res);
        free(value);
    } else {
        res = common_getxattr(f, ino, name, NULL, 0);
        if (res >= 0)
            fuse_reply_xattr(req, res);
        else
            reply_err(req, res);
    }
}

static int common_listxattr(struct fuse *f, fuse_ino_t ino, char *list,
                            size_t size)
{
    char *path;
    int err;

    err = -ENOENT;
    pthread_rwlock_rdlock(&f->tree_lock);
    path = get_path(f, ino);
    if (path != NULL) {
        err = -ENOSYS;
        if (f->op.listxattr)
            err = f->op.listxattr(path, list, size);
        free(path);
    }
    pthread_rwlock_unlock(&f->tree_lock);
    return err;
}

static void fuse_listxattr(fuse_req_t req, fuse_ino_t ino, size_t size)
{
    struct fuse *f = req_fuse_prepare(req);
    int res;

    if (size) {
        char *list = (char *) malloc(size);
        if (list == NULL) {
            reply_err(req, -ENOMEM);
            return;
        }
        res = common_listxattr(f, ino, list, size);
        if (res > 0)
            fuse_reply_buf(req, list, res);
        else
            reply_err(req, res);
        free(list);
    } else {
        res = common_listxattr(f, ino, NULL, 0);
        if (res >= 0)
            fuse_reply_xattr(req, res);
        else
            reply_err(req, res);
    }
}

static void fuse_removexattr(fuse_req_t req, fuse_ino_t ino, const char *name)
{
    struct fuse *f = req_fuse_prepare(req);
    char *path;
    int err;

    err = -ENOENT;
    pthread_rwlock_rdlock(&f->tree_lock);
    path = get_path(f, ino);
    if (path != NULL) {
        err = -ENOSYS;
        if (f->op.removexattr)
            err = f->op.removexattr(path, name);
        free(path);
    }
    pthread_rwlock_unlock(&f->tree_lock);
    reply_err(req, err);
}

static struct fuse_lowlevel_ops fuse_path_ops = {
    .init = fuse_data_init,
    .destroy = fuse_data_destroy,
    .lookup = fuse_lookup,
    .forget = fuse_forget,
    .getattr = fuse_getattr,
    .setattr = fuse_setattr,
    .access = fuse_access,
    .readlink = fuse_readlink,
    .mknod = fuse_mknod,
    .mkdir = fuse_mkdir,
    .unlink = fuse_unlink,
    .rmdir = fuse_rmdir,
    .symlink = fuse_symlink,
    .rename = fuse_rename,
    .link = fuse_link,
    .create = fuse_create,
    .open = fuse_open,
    .read = fuse_read,
    .write = fuse_write,
    .flush = fuse_flush,
    .release = fuse_release,
    .fsync = fuse_fsync,
    .opendir = fuse_opendir,
    .readdir = fuse_readdir,
    .releasedir = fuse_releasedir,
    .fsyncdir = fuse_fsyncdir,
    .statfs = fuse_statfs,
    .setxattr = fuse_setxattr,
    .getxattr = fuse_getxattr,
    .listxattr = fuse_listxattr,
    .removexattr = fuse_removexattr,
};


struct fuse *
glusterfs_fuse_new_common(int fd, 
			  struct fuse_args *args)
{
    struct fuse_chan *ch;
    struct fuse *f;
    struct node *root;
    int compat = 0;

    f = (struct fuse *) calloc(1, sizeof(struct fuse));
    if (f == NULL) {
        fprintf(stderr, "fuse: failed to allocate fuse object\n");
        goto out;
    }

    f->conf.entry_timeout = 1.0;
    f->conf.attr_timeout = 1.0;
    f->conf.negative_timeout = 0.0;

    {
      f->conf.uid = 0;
      f->conf.gid = 0;
      f->conf.umask = 0;
      f->conf.debug = 1;
      f->conf.hard_remove = 1;
      f->conf.use_ino = 0;
      f->conf.readdir_ino = 0;
      f->conf.set_mode = 0;
      f->conf.set_uid = 0;
      f->conf.set_gid = 0;
      f->conf.direct_io = 0;
      f->conf.kernel_cache = 0;
    }

    f->se = fuse_lowlevel_new(args, &fuse_path_ops, sizeof(fuse_path_ops), f);
    if (f->se == NULL)
        goto out_free;

    ch = fuse_kern_chan_new(fd);
    if (ch == NULL)
        goto out_free_session;

    fuse_session_add_chan(f->se, ch);

    f->ctr = 0;
    f->generation = 0;
    /* FIXME: Dynamic hash table */
    f->name_table_size = 14057;
    f->name_table = (struct node **)
        calloc(1, sizeof(struct node *) * f->name_table_size);
    if (f->name_table == NULL) {
        fprintf(stderr, "fuse: memory allocation failed\n");
        goto out_free_session;
    }

    f->id_table_size = 14057;
    f->id_table = (struct node **)
        calloc(1, sizeof(struct node *) * f->id_table_size);
    if (f->id_table == NULL) {
        fprintf(stderr, "fuse: memory allocation failed\n");
        goto out_free_name_table;
    }

    pthread_mutex_init(&f->lock, NULL);
    pthread_rwlock_init(&f->tree_lock, NULL);
    //    memcpy(&f->op, op, op_size);
    f->compat = compat;

    root = (struct node *) calloc(1, sizeof(struct node));
    if (root == NULL) {
        fprintf(stderr, "fuse: memory allocation failed\n");
        goto out_free_id_table;
    }

    root->name = strdup("/");
    if (root->name == NULL) {
        fprintf(stderr, "fuse: memory allocation failed\n");
        goto out_free_root;
    }

    root->parent = 0;
    root->nodeid = FUSE_ROOT_ID;
    root->generation = 0;
    root->refctr = 1;
    root->nlookup = 1;
    hash_id(f, root);

    return f;

 out_free_root:
    free(root);
 out_free_id_table:
    free(f->id_table);
 out_free_name_table:
    free(f->name_table);
 out_free_session:
    fuse_session_destroy(f->se);
 out_free:
    free(f);
 out:
    return NULL;
}