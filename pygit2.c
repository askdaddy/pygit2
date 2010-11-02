/*
 * Copyright 2010 Google, Inc.
 *
 * This file is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 2,
 * as published by the Free Software Foundation.
 *
 * In addition to the permissions in the GNU General Public License,
 * the authors give you unlimited permission to link the compiled
 * version of this file into combinations with other programs,
 * and to distribute those combinations without any restriction
 * coming from the use of this file.  (The General Public License
 * restrictions do apply in other respects; for example, they cover
 * modification of the file, and distribution when not linked into
 * a combined executable.)
 *
 * This file is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; see the file COPYING.  If not, write to
 * the Free Software Foundation, 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include <Python.h>
#include <git/commit.h>
#include <git/common.h>
#include <git/errors.h>
#include <git/repository.h>
#include <git/commit.h>
#include <git/odb.h>
#include <git/tag.h>

typedef struct {
    PyObject_HEAD
    git_repository *repo;
} Repository;

/* The structs for some of the object subtypes are identical except for the type
 * of their object pointers. */
#define OBJECT_STRUCT(_name, _ptr_type, _ptr_name) \
        typedef struct {\
            PyObject_HEAD\
            Repository *repo;\
            int own_obj:1;\
            _ptr_type *_ptr_name;\
        } _name;

OBJECT_STRUCT(Object, git_object, obj)
OBJECT_STRUCT(Commit, git_commit, commit)
OBJECT_STRUCT(Tree, git_tree, tree)
OBJECT_STRUCT(Blob, git_object, blob)

typedef struct {
    PyObject_HEAD
    Repository *repo;
    int own_obj:1;
    git_tag *tag;
    Object *target;
} Tag;

typedef struct {
    PyObject_HEAD
    git_tree_entry *entry;
    Tree *tree;
} TreeEntry;

static PyTypeObject RepositoryType;
static PyTypeObject ObjectType;
static PyTypeObject CommitType;
static PyTypeObject TreeEntryType;
static PyTypeObject TreeType;
static PyTypeObject BlobType;
static PyTypeObject TagType;

static PyObject *GitError;

static PyObject *
Error_set(int err) {
    assert(err < 0);
    switch (err) {
        case GIT_ENOMEM:
            PyErr_NoMemory();
            break;
        case GIT_EOSERR:
            PyErr_SetFromErrno(GitError);
            break;
        case GIT_EOBJTYPE:
            PyErr_SetString(GitError, "Invalid object type.");
            break;
        default:
            PyErr_Format(GitError, "%s", git_strerror(err));
            break;
    }
    return NULL;
}

static PyObject *
Error_set_py_str(int err, PyObject *py_str) {
    PyObject *repr = NULL;

    assert(err < 0);
    switch (err) {
        case GIT_ENOTOID:
            if (!PyString_Check(py_str)) {
                PyErr_Format(PyExc_TypeError, "Hex SHA must be str, not %.200s",
                             py_str->ob_type->tp_name);
            } else {
                repr = PyObject_Repr(py_str);
                if (repr)
                    PyErr_Format(PyExc_ValueError, "Invalid hex SHA: %s",
                                 PyString_AS_STRING(repr));
                else
                    PyErr_SetString(PyExc_ValueError,
                                    "Invalid hex SHA: <error in __repr__>");
            }
            break;

        case GIT_ENOTFOUND:
            PyErr_SetObject(PyExc_KeyError, py_str);
            break;

        case GIT_EOBJCORRUPTED:
            repr = PyObject_Str(py_str);
            if (repr)
                PyErr_Format(GitError, "Corrupted object: %s",
                             PyString_AS_STRING(repr));
            else
                PyErr_SetString(GitError,
                                "Corrupted object: <error in __str__>");
            break;

        default:
            Error_set(err);
            break;
    }
    Py_XDECREF(repr);
    return NULL;
}

static int
py_str_to_git_oid(PyObject *py_str, git_oid *oid) {
    char *hex;
    hex = PyString_AsString(py_str);
    if (!hex)
        return GIT_ENOTOID;
    return git_oid_mkstr(oid, hex);
}

static int
Repository_init(Repository *self, PyObject *args, PyObject *kwds) {
    char *path;

    if (kwds) {
        PyErr_SetString(PyExc_TypeError,
                        "Repository takes no keyword arugments");
        return -1;
    }

    if (!PyArg_ParseTuple(args, "s", &path))
        return -1;

    self->repo = git_repository_open(path);
    if (!self->repo) {
        PyErr_Format(PyExc_RuntimeError, "Failed to open repo directory at %s",
                     path);
        return -1;
    }

    return 0;
}

static void
Repository_dealloc(Repository *self) {
    if (self->repo)
        git_repository_free(self->repo);
    self->ob_type->tp_free((PyObject*)self);
}

static int
Repository_contains(Repository *self, PyObject *value) {
    git_oid oid;
    int err;
    err = py_str_to_git_oid(value, &oid);
    if (err < 0) {
        Error_set_py_str(err, value);
        return -1;
    }
    return git_odb_exists(git_repository_database(self->repo), &oid);
}

static Object *wrap_object(git_object *, Repository *repo);

static Tag *
wrap_tag(git_tag *tag, Repository *repo) {
    Tag *py_tag;
    Object *py_target;

    py_tag = (Tag*)TagType.tp_alloc(&TagType, 0);
    if (!py_tag)
        return NULL;

    py_target = wrap_object((git_object*)git_tag_target(tag), repo);
    if (!py_target) {
        py_tag->ob_type->tp_free((PyObject*)py_tag);
        return NULL;
    }
    py_tag->target = py_target;
    Py_INCREF(py_target);
    return py_tag;
}

static Object *
wrap_object(git_object *obj, Repository *repo) {
    Object *py_obj = NULL;
    switch (git_object_type(obj)) {
        case GIT_OBJ_COMMIT:
            py_obj = (Object*)CommitType.tp_alloc(&CommitType, 0);
            break;
        case GIT_OBJ_TREE:
            py_obj = (Object*)TreeType.tp_alloc(&TreeType, 0);
            break;
        case GIT_OBJ_BLOB:
            py_obj = (Object*)BlobType.tp_alloc(&BlobType, 0);
            break;
        case GIT_OBJ_TAG:
            py_obj = (Object*)wrap_tag((git_tag*)obj, repo);
            break;
        default:
            assert(0);
    }
    if (!py_obj)
        return (Object*)PyErr_NoMemory();

    py_obj->obj = obj;
    py_obj->repo = repo;
    Py_INCREF(repo);
    return py_obj;
}

static PyObject *
Repository_getitem(Repository *self, PyObject *value) {
    git_oid oid;
    int err;
    git_object *obj;
    Object *py_obj;

    err = py_str_to_git_oid(value, &oid);
    if (err < 0)
        return Error_set_py_str(err, value);

    obj = git_repository_lookup(self->repo, &oid, GIT_OBJ_ANY);
    /* Grr, libgit2 currently doesn't give us any info on this error, so we
     * can't even tell the difference between a missing object and, say, an I/O
     * error. */
    if (!obj)
        return Error_set_py_str(GIT_ENOTFOUND, value);

    py_obj = wrap_object(obj, self);
    if (!py_obj)
        return NULL;
    py_obj->own_obj = 0;
    return (PyObject*)py_obj;
}

static int
Repository_read_raw(git_rawobj *raw, git_repository *repo, const git_oid *oid) {
    return git_odb_read(raw, git_repository_database(repo), oid);
}

static PyObject *
Repository_read(Repository *self, PyObject *py_hex) {
    git_oid oid;
    int err;
    git_rawobj raw;
    PyObject *result;

    err = py_str_to_git_oid(py_hex, &oid);
    if (err < 0)
        return Error_set_py_str(err, py_hex);

    err = Repository_read_raw(&raw, self->repo, &oid);
    if (err < 0)
        return Error_set_py_str(err, py_hex);

    result = Py_BuildValue("(ns#)", raw.type, raw.data, raw.len);
    free(raw.data);
    return result;
}

static PyMethodDef Repository_methods[] = {
    {"read", (PyCFunction)Repository_read, METH_O,
     "Read raw object data from the repository."},
    {NULL, NULL, 0, NULL}
};

static PySequenceMethods Repository_as_sequence = {
    0,                               /* sq_length */
    0,                               /* sq_concat */
    0,                               /* sq_repeat */
    0,                               /* sq_item */
    0,                               /* sq_slice */
    0,                               /* sq_ass_item */
    0,                               /* sq_ass_slice */
    (objobjproc)Repository_contains, /* sq_contains */
};

static PyMappingMethods Repository_as_mapping = {
    0,                               /* mp_length */
    (binaryfunc)Repository_getitem,  /* mp_subscript */
    0,                               /* mp_ass_subscript */
};

static PyTypeObject RepositoryType = {
    PyObject_HEAD_INIT(NULL)
    0,                                         /* ob_size */
    "pygit2.Repository",                       /* tp_name */
    sizeof(Repository),                        /* tp_basicsize */
    0,                                         /* tp_itemsize */
    (destructor)Repository_dealloc,            /* tp_dealloc */
    0,                                         /* tp_print */
    0,                                         /* tp_getattr */
    0,                                         /* tp_setattr */
    0,                                         /* tp_compare */
    0,                                         /* tp_repr */
    0,                                         /* tp_as_number */
    &Repository_as_sequence,                   /* tp_as_sequence */
    &Repository_as_mapping,                    /* tp_as_mapping */
    0,                                         /* tp_hash  */
    0,                                         /* tp_call */
    0,                                         /* tp_str */
    0,                                         /* tp_getattro */
    0,                                         /* tp_setattro */
    0,                                         /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,  /* tp_flags */
    "Git repository",                          /* tp_doc */
    0,                                         /* tp_traverse */
    0,                                         /* tp_clear */
    0,                                         /* tp_richcompare */
    0,                                         /* tp_weaklistoffset */
    0,                                         /* tp_iter */
    0,                                         /* tp_iternext */
    Repository_methods,                        /* tp_methods */
    0,                                         /* tp_members */
    0,                                         /* tp_getset */
    0,                                         /* tp_base */
    0,                                         /* tp_dict */
    0,                                         /* tp_descr_get */
    0,                                         /* tp_descr_set */
    0,                                         /* tp_dictoffset */
    (initproc)Repository_init,                 /* tp_init */
    0,                                         /* tp_alloc */
    0,                                         /* tp_new */
};

static void
Object_dealloc(Object* self)
{
    if (self->own_obj)
        git_object_free(self->obj);
    Py_XDECREF(self->repo);
    self->ob_type->tp_free((PyObject*)self);
}

static PyObject *
Object_get_type(Object *self) {
    return PyInt_FromLong(git_object_type(self->obj));
}

static PyObject *
Object_get_sha(Object *self) {
    const git_oid *id;
    char hex[GIT_OID_HEXSZ];

    id = git_object_id(self->obj);
    if (!id)
        Py_RETURN_NONE;

    git_oid_fmt(hex, id);
    return PyString_FromStringAndSize(hex, GIT_OID_HEXSZ);
}

static PyObject *
Object_read_raw(Object *self) {
    const git_oid *id;
    git_rawobj raw;
    int err;
    PyObject *result = NULL, *py_sha = NULL;

    id = git_object_id(self->obj);
    if (!id)
        Py_RETURN_NONE;  /* in-memory object */

    err = Repository_read_raw(&raw, self->repo->repo, id);
    if (err < 0) {
        py_sha = Object_get_sha(self);
        Error_set_py_str(err, py_sha);
        goto cleanup;
    }

    result = PyString_FromStringAndSize(raw.data, raw.len);

cleanup:
    Py_XDECREF(py_sha);
    free(raw.data);
    return result;
}

static PyObject *
Object_write(Object *self) {
    int err;
    PyObject *py_sha;
    err = git_object_write(self->obj);
    if (err < 0) {
        py_sha = Object_get_sha(self);
        Error_set_py_str(err, py_sha);
        Py_DECREF(py_sha);
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyGetSetDef Object_getseters[] = {
    {"type", (getter)Object_get_type, NULL, "type number", NULL},
    {"sha", (getter)Object_get_sha, NULL, "hex SHA", NULL},
    {NULL}
};

static PyMethodDef Object_methods[] = {
    {"read_raw", (PyCFunction)Object_read_raw, METH_NOARGS,
     "Read the raw contents of the object from the repo."},
    {"write", (PyCFunction)Object_write, METH_NOARGS,
     "Write the object to the repo, if changed."},
    {NULL}
};

static PyTypeObject ObjectType = {
    PyObject_HEAD_INIT(NULL)
    0,                                         /*ob_size*/
    "pygit2.Object",                           /*tp_name*/
    sizeof(Object),                            /*tp_basicsize*/
    0,                                         /*tp_itemsize*/
    (destructor)Object_dealloc,                /*tp_dealloc*/
    0,                                         /*tp_print*/
    0,                                         /*tp_getattr*/
    0,                                         /*tp_setattr*/
    0,                                         /*tp_compare*/
    0,                                         /*tp_repr*/
    0,                                         /*tp_as_number*/
    0,                                         /*tp_as_sequence*/
    0,                                         /*tp_as_mapping*/
    0,                                         /*tp_hash */
    0,                                         /*tp_call*/
    0,                                         /*tp_str*/
    0,                                         /*tp_getattro*/
    0,                                         /*tp_setattro*/
    0,                                         /*tp_as_buffer*/
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,  /*tp_flags*/
    "Object objects",                          /* tp_doc */
    0,                                         /* tp_traverse */
    0,                                         /* tp_clear */
    0,                                         /* tp_richcompare */
    0,                                         /* tp_weaklistoffset */
    0,                                         /* tp_iter */
    0,                                         /* tp_iternext */
    Object_methods,                            /* tp_methods */
    0,                                         /* tp_members */
    Object_getseters,                          /* tp_getset */
    0,                                         /* tp_base */
    0,                                         /* tp_dict */
    0,                                         /* tp_descr_get */
    0,                                         /* tp_descr_set */
    0,                                         /* tp_dictoffset */
    0,                                         /* tp_init */
    0,                                         /* tp_alloc */
    0,                                         /* tp_new */
};

static int
Object_init_with_type(Object *py_obj, const git_otype type, PyObject *args,
                      PyObject *kwds) {
    Repository *repo = NULL;
    git_object *obj;

    if (kwds) {
        PyErr_Format(PyExc_TypeError, "%s takes no keyword arugments",
                     py_obj->ob_type->tp_name);
        return -1;
    }

    if (!PyArg_ParseTuple(args, "O", &repo))
        return -1;

    if (!PyObject_TypeCheck(repo, &RepositoryType)) {
        PyErr_Format(PyExc_TypeError,
                     "repo argument must be %.200s, not %.200s",
                     RepositoryType.tp_name, repo->ob_type->tp_name);
        return -1;
    }

    obj = git_object_new(repo->repo, type);
    if (!obj) {
        PyErr_SetNone(PyExc_MemoryError);
        return -1;
    }
    Py_INCREF(repo);
    py_obj->repo = repo;
    py_obj->own_obj = 1;
    py_obj->obj = obj;
    return 0;
}

static int
Commit_init(Commit *py_commit, PyObject *args, PyObject *kwds) {
    return Object_init_with_type((Object*)py_commit, GIT_OBJ_COMMIT, args,
                                 kwds);
}

static PyObject *
Commit_get_message_short(Commit *commit) {
    return PyString_FromString(git_commit_message_short(commit->commit));
}

static PyObject *
Commit_get_message(Commit *commit) {
    return PyString_FromString(git_commit_message(commit->commit));
}

static int
Commit_set_message(Commit *commit, PyObject *message) {
    if (!PyString_Check(message)) {
        PyErr_Format(PyExc_TypeError, "message must be str, not %.200s",
                     message->ob_type->tp_name);
        return -1;
    }
    git_commit_set_message(commit->commit, PyString_AS_STRING(message));
    return 0;
}

static PyObject *
Commit_get_commit_time(Commit *commit) {
    return PyLong_FromLong(git_commit_time(commit->commit));
}

static PyObject *
Commit_get_committer(Commit *commit) {
    git_person *committer;
    committer = (git_person*)git_commit_committer(commit->commit);
    return Py_BuildValue("(ssl)", git_person_name(committer),
                         git_person_email(committer),
                         git_person_time(committer));
}

static int
Commit_set_committer(Commit *commit, PyObject *value) {
    char *name = NULL, *email = NULL;
    long long time;
    if (!PyArg_ParseTuple(value, "ssL", &name, &email, &time))
        return -1;
    git_commit_set_committer(commit->commit, name, email, time);
    return 0;
}

static PyObject *
Commit_get_author(Commit *commit) {
    git_person *author;
    author = (git_person*)git_commit_author(commit->commit);
    return Py_BuildValue("(ssl)", git_person_name(author),
                         git_person_email(author),
                         git_person_time(author));
}

static int
Commit_set_author(Commit *commit, PyObject *value) {
    char *name = NULL, *email = NULL;
    long long time;
    if (!PyArg_ParseTuple(value, "ssL", &name, &email, &time))
        return -1;
    git_commit_set_author(commit->commit, name, email, time);
    return 0;
}

static PyGetSetDef Commit_getseters[] = {
    {"message_short", (getter)Commit_get_message_short, NULL, "short message",
     NULL},
    {"message", (getter)Commit_get_message, (setter)Commit_set_message,
     "message", NULL},
    {"commit_time", (getter)Commit_get_commit_time, NULL, "commit time",
     NULL},
    {"committer", (getter)Commit_get_committer,
     (setter)Commit_set_committer, "committer", NULL},
    {"author", (getter)Commit_get_author,
     (setter)Commit_set_author, "author", NULL},
    {NULL}
};

static PyTypeObject CommitType = {
    PyObject_HEAD_INIT(NULL)
    0,                                         /*ob_size*/
    "pygit2.Commit",                           /*tp_name*/
    sizeof(Commit),                            /*tp_basicsize*/
    0,                                         /*tp_itemsize*/
    0,                                         /*tp_dealloc*/
    0,                                         /*tp_print*/
    0,                                         /*tp_getattr*/
    0,                                         /*tp_setattr*/
    0,                                         /*tp_compare*/
    0,                                         /*tp_repr*/
    0,                                         /*tp_as_number*/
    0,                                         /*tp_as_sequence*/
    0,                                         /*tp_as_mapping*/
    0,                                         /*tp_hash */
    0,                                         /*tp_call*/
    0,                                         /*tp_str*/
    0,                                         /*tp_getattro*/
    0,                                         /*tp_setattro*/
    0,                                         /*tp_as_buffer*/
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,  /*tp_flags*/
    "Commit objects",                          /* tp_doc */
    0,                                         /* tp_traverse */
    0,                                         /* tp_clear */
    0,                                         /* tp_richcompare */
    0,                                         /* tp_weaklistoffset */
    0,                                         /* tp_iter */
    0,                                         /* tp_iternext */
    0,                                         /* tp_methods */
    0,                                         /* tp_members */
    Commit_getseters,                          /* tp_getset */
    0,                                         /* tp_base */
    0,                                         /* tp_dict */
    0,                                         /* tp_descr_get */
    0,                                         /* tp_descr_set */
    0,                                         /* tp_dictoffset */
    (initproc)Commit_init,                     /* tp_init */
    0,                                         /* tp_alloc */
    0,                                         /* tp_new */
};

static void
TreeEntry_dealloc(TreeEntry *self) {
    Py_XDECREF(self->tree);
    self->ob_type->tp_free((PyObject *)self);
}

static PyObject *
TreeEntry_get_attributes(TreeEntry *self) {
    return PyInt_FromLong(git_tree_entry_attributes(self->entry));
}

static int
TreeEntry_set_attributes(TreeEntry *self, PyObject *value) {
    unsigned int attributes;
    attributes = PyInt_AsLong(value);
    if (PyErr_Occurred())
        return -1;
    git_tree_entry_set_attributes(self->entry, attributes);
    return 0;
}

static PyObject *
TreeEntry_get_name(TreeEntry *self) {
    return PyString_FromString(git_tree_entry_name(self->entry));
}

static int
TreeEntry_set_name(TreeEntry *self, PyObject *value) {
    char *name;
    name = PyString_AsString(value);
    if (!name)
        return -1;
    git_tree_entry_set_name(self->entry, name);
    return 0;
}

static PyObject *
TreeEntry_get_sha(TreeEntry *self) {
    char hex[GIT_OID_HEXSZ];
    git_oid_fmt(hex, git_tree_entry_id(self->entry));
    return PyString_FromStringAndSize(hex, GIT_OID_HEXSZ);
}

static int
TreeEntry_set_sha(TreeEntry *self, PyObject *value) {
    git_oid oid;
    int err;

    err = py_str_to_git_oid(value, &oid);
    if (err < 0) {
        Error_set_py_str(err, value);
        return -1;
    }
    git_tree_entry_set_id(self->entry, &oid);
    return 0;
}

static PyObject *
TreeEntry_to_object(TreeEntry *self) {
    git_object *obj;
    char hex[GIT_OID_HEXSZ];
    PyObject *py_hex;

    obj = git_tree_entry_2object(self->entry);
    if (!obj) {
        git_oid_fmt(hex, git_tree_entry_id(self->entry));
        py_hex = PyString_FromStringAndSize(hex, GIT_OID_HEXSZ);
        PyErr_SetObject(PyExc_KeyError, py_hex);
        return NULL;
    }
    return (PyObject*)wrap_object(obj, self->tree->repo);
}

static PyGetSetDef TreeEntry_getseters[] = {
    {"attributes", (getter)TreeEntry_get_attributes,
     (setter)TreeEntry_set_attributes, "attributes", NULL},
    {"name", (getter)TreeEntry_get_name, (setter)TreeEntry_set_name, "name",
     NULL},
    {"sha", (getter)TreeEntry_get_sha, (setter)TreeEntry_set_sha, "sha", NULL},
    {NULL}
};

static PyMethodDef TreeEntry_methods[] = {
    {"to_object", (PyCFunction)TreeEntry_to_object, METH_NOARGS,
     "Look up the corresponding object in the repo."},
    {NULL, NULL, 0, NULL}
};

static PyTypeObject TreeEntryType = {
    PyObject_HEAD_INIT(NULL)
    0,                                         /*ob_size*/
    "pygit2.TreeEntry",                        /*tp_name*/
    sizeof(TreeEntry),                         /*tp_basicsize*/
    0,                                         /*tp_itemsize*/
    (destructor)TreeEntry_dealloc,             /*tp_dealloc*/
    0,                                         /*tp_print*/
    0,                                         /*tp_getattr*/
    0,                                         /*tp_setattr*/
    0,                                         /*tp_compare*/
    0,                                         /*tp_repr*/
    0,                                         /*tp_as_number*/
    0,                                         /*tp_as_sequence*/
    0,                                         /*tp_as_mapping*/
    0,                                         /*tp_hash */
    0,                                         /*tp_call*/
    0,                                         /*tp_str*/
    0,                                         /*tp_getattro*/
    0,                                         /*tp_setattro*/
    0,                                         /*tp_as_buffer*/
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,  /*tp_flags*/
    "TreeEntry objects",                       /* tp_doc */
    0,                                         /* tp_traverse */
    0,                                         /* tp_clear */
    0,                                         /* tp_richcompare */
    0,                                         /* tp_weaklistoffset */
    0,                                         /* tp_iter */
    0,                                         /* tp_iternext */
    TreeEntry_methods,                         /* tp_methods */
    0,                                         /* tp_members */
    TreeEntry_getseters,                       /* tp_getset */
    0,                                         /* tp_base */
    0,                                         /* tp_dict */
    0,                                         /* tp_descr_get */
    0,                                         /* tp_descr_set */
    0,                                         /* tp_dictoffset */
    0,                                         /* tp_init */
    0,                                         /* tp_alloc */
    0,                                         /* tp_new */
};

static int
Tree_init(Tree *py_tree, PyObject *args, PyObject *kwds) {
    return Object_init_with_type((Object*)py_tree, GIT_OBJ_TREE, args, kwds);
}

static Py_ssize_t
Tree_len(Tree *self) {
    return (Py_ssize_t)git_tree_entrycount(self->tree);
}

static int
Tree_contains(Tree *self, PyObject *py_name) {
    char *name;
    name = PyString_AsString(py_name);
    return name && git_tree_entry_byname(self->tree, name) ? 1 : 0;
}

static TreeEntry *
wrap_tree_entry(git_tree_entry *entry, Tree *tree) {
    TreeEntry *py_entry = NULL;
    py_entry = (TreeEntry*)TreeEntryType.tp_alloc(&TreeEntryType, 0);
    if (!py_entry)
        return NULL;

    py_entry->entry = entry;
    py_entry->tree = tree;
    Py_INCREF(tree);
    return py_entry;
}

static TreeEntry *
Tree_getitem_by_name(Tree *self, PyObject *py_name) {
    char *name;
    git_tree_entry *entry;
    name = PyString_AS_STRING(py_name);
    entry = git_tree_entry_byname(self->tree, name);
    if (!entry) {
        PyErr_SetObject(PyExc_KeyError, py_name);
        return NULL;
    }
    return wrap_tree_entry(entry, self);
}

static int
Tree_fix_index(Tree *self, PyObject *py_index) {
    long index;
    size_t len;
    long slen;

    index = PyInt_AsLong(py_index);
    if (PyErr_Occurred())
        return -1;

    len = git_tree_entrycount(self->tree);
    slen = (long)len;
    if (index >= slen) {
        PyErr_SetObject(PyExc_IndexError, py_index);
        return -1;
    } else if (index < -slen) {
        PyErr_SetObject(PyExc_IndexError, py_index);
        return -1;
    }

    /* This function is called via mp_subscript, which doesn't do negative index
     * rewriting, so we have to do it manually. */
    if (index < 0)
        index = len + index;
    return (int)index;
}

static TreeEntry *
Tree_getitem_by_index(Tree *self, PyObject *py_index) {
    int index;
    git_tree_entry *entry;

    index = Tree_fix_index(self, py_index);
    if (PyErr_Occurred())
        return NULL;

    entry = git_tree_entry_byindex(self->tree, index);
    if (!entry) {
        PyErr_SetObject(PyExc_IndexError, py_index);
        return NULL;
    }
    return wrap_tree_entry(entry, self);
}

static TreeEntry *
Tree_getitem(Tree *self, PyObject *value) {
    if (PyString_Check(value)) {
        return Tree_getitem_by_name(self, value);
    } else if (PyInt_Check(value)) {
        return Tree_getitem_by_index(self, value);
    } else {
        PyErr_SetString(PyExc_TypeError, "Expected int or str for tree index.");
        return NULL;
    }
}

static int
Tree_delitem_by_name(Tree *self, PyObject *name) {
    int err;
    err = git_tree_remove_entry_byname(self->tree, PyString_AS_STRING(name));
    if (err < 0) {
        PyErr_SetObject(PyExc_KeyError, name);
        return -1;
    }
    return 0;
}

static int
Tree_delitem_by_index(Tree *self, PyObject *py_index) {
    int index, err;
    index = Tree_fix_index(self, py_index);
    if (PyErr_Occurred())
        return -1;
    err = git_tree_remove_entry_byindex(self->tree, index);
    if (err < 0) {
        PyErr_SetObject(PyExc_IndexError, py_index);
        return -1;
    }
    return 0;
}

static int
Tree_delitem(Tree *self, PyObject *name, PyObject *value) {
    /* TODO: This function is only used for deleting items. We may be able to
     * come up with some reasonable assignment semantics, but it's tricky
     * because git_tree_entry objects are owned by their containing tree. */
    if (value) {
        PyErr_SetString(PyExc_ValueError,
                        "Cannot set TreeEntry directly; use add_entry.");
        return -1;
    }

    if (PyString_Check(name)) {
        return Tree_delitem_by_name(self, name);
    } else if (PyInt_Check(name)) {
        return Tree_delitem_by_index(self, name);
    } else {
        PyErr_SetString(PyExc_TypeError, "Expected int or str for tree index.");
        return -1;
    }
}

static PyObject *
Tree_add_entry(Tree *self, PyObject *args) {
    PyObject *py_sha;
    char *name;
    int attributes, err;
    git_oid oid;

    if (!PyArg_ParseTuple(args, "Osi", &py_sha, &name, &attributes))
        return NULL;

    err = py_str_to_git_oid(py_sha, &oid);
    if (err < 0)
        return Error_set_py_str(err, py_sha);

    if (git_tree_add_entry(self->tree, &oid, name, attributes) < 0)
        return PyErr_NoMemory();
    Py_RETURN_NONE;
}

static PyMethodDef Tree_methods[] = {
    {"add_entry", (PyCFunction)Tree_add_entry, METH_VARARGS,
     "Add an entry to a Tree."},
    {NULL}
};

static PySequenceMethods Tree_as_sequence = {
    0,                          /* sq_length */
    0,                          /* sq_concat */
    0,                          /* sq_repeat */
    0,                          /* sq_item */
    0,                          /* sq_slice */
    0,                          /* sq_ass_item */
    0,                          /* sq_ass_slice */
    (objobjproc)Tree_contains,  /* sq_contains */
};

static PyMappingMethods Tree_as_mapping = {
    (lenfunc)Tree_len,            /* mp_length */
    (binaryfunc)Tree_getitem,     /* mp_subscript */
    (objobjargproc)Tree_delitem,  /* mp_ass_subscript */
};

static PyTypeObject TreeType = {
    PyObject_HEAD_INIT(NULL)
    0,                                         /*ob_size*/
    "pygit2.Tree",                             /*tp_name*/
    sizeof(Tree),                              /*tp_basicsize*/
    0,                                         /*tp_itemsize*/
    0,                                         /*tp_dealloc*/
    0,                                         /*tp_print*/
    0,                                         /*tp_getattr*/
    0,                                         /*tp_setattr*/
    0,                                         /*tp_compare*/
    0,                                         /*tp_repr*/
    0,                                         /*tp_as_number*/
    &Tree_as_sequence,                         /*tp_as_sequence*/
    &Tree_as_mapping,                          /*tp_as_mapping*/
    0,                                         /*tp_hash */
    0,                                         /*tp_call*/
    0,                                         /*tp_str*/
    0,                                         /*tp_getattro*/
    0,                                         /*tp_setattro*/
    0,                                         /*tp_as_buffer*/
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,  /*tp_flags*/
    "Tree objects",                            /* tp_doc */
    0,                                         /* tp_traverse */
    0,                                         /* tp_clear */
    0,                                         /* tp_richcompare */
    0,                                         /* tp_weaklistoffset */
    0,                                         /* tp_iter */
    0,                                         /* tp_iternext */
    Tree_methods,                              /* tp_methods */
    0,                                         /* tp_members */
    0,                                         /* tp_getset */
    0,                                         /* tp_base */
    0,                                         /* tp_dict */
    0,                                         /* tp_descr_get */
    0,                                         /* tp_descr_set */
    0,                                         /* tp_dictoffset */
    (initproc)Tree_init,                       /* tp_init */
    0,                                         /* tp_alloc */
    0,                                         /* tp_new */
};

static int
Blob_init(Blob *py_blob, PyObject *args, PyObject *kwds) {
    return Object_init_with_type((Object*)py_blob, GIT_OBJ_BLOB, args, kwds);
}

/* TODO: libgit2 needs some way to set blob data. */
static PyGetSetDef Blob_getseters[] = {
    {"data", (getter)Object_read_raw, NULL, "raw data", NULL},
    {NULL}
};

static PyTypeObject BlobType = {
    PyObject_HEAD_INIT(NULL)
    0,                                         /*ob_size*/
    "pygit2.Blob",                             /*tp_name*/
    sizeof(Blob),                              /*tp_basicsize*/
    0,                                         /*tp_itemsize*/
    0,                                         /*tp_dealloc*/
    0,                                         /*tp_print*/
    0,                                         /*tp_getattr*/
    0,                                         /*tp_setattr*/
    0,                                         /*tp_compare*/
    0,                                         /*tp_repr*/
    0,                                         /*tp_as_number*/
    0,                                         /*tp_as_sequence*/
    0,                                         /*tp_as_mapping*/
    0,                                         /*tp_hash */
    0,                                         /*tp_call*/
    0,                                         /*tp_str*/
    0,                                         /*tp_getattro*/
    0,                                         /*tp_setattro*/
    0,                                         /*tp_as_buffer*/
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,  /*tp_flags*/
    "Blob objects",                            /* tp_doc */
    0,                                         /* tp_traverse */
    0,                                         /* tp_clear */
    0,                                         /* tp_richcompare */
    0,                                         /* tp_weaklistoffset */
    0,                                         /* tp_iter */
    0,                                         /* tp_iternext */
    0,                                         /* tp_methods */
    0,                                         /* tp_members */
    Blob_getseters,                            /* tp_getset */
    0,                                         /* tp_base */
    0,                                         /* tp_dict */
    0,                                         /* tp_descr_get */
    0,                                         /* tp_descr_set */
    0,                                         /* tp_dictoffset */
    (initproc)Blob_init,                       /* tp_init */
    0,                                         /* tp_alloc */
    0,                                         /* tp_new */
};

static int
Tag_init(Tag *py_tag, PyObject *args, PyObject *kwds) {
    return Object_init_with_type((Object*)py_tag, GIT_OBJ_TAG, args, kwds);
}

static void
Tag_dealloc(Tag *self) {
    Py_XDECREF(self->target);
    self->ob_type->tp_free((PyObject*)self);
}

static PyObject *
Tag_get_target(Tag *self) {
    git_object *target;
    target = (git_object*)git_tag_target(self->tag);
    if (!target) {
        /* This can only happen if we have a new tag with no target set yet. In
         * particular, it can't happen if the tag fails to parse, since that
         * would have returned NULL from git_repository_lookup. */
        Py_RETURN_NONE;
    }
    return (PyObject*)wrap_object(target, self->repo);
}

static int
Tag_set_target(Tag *self, Object *target) {
    if (!PyObject_TypeCheck(target, &ObjectType)) {
        PyErr_Format(PyExc_TypeError, "target must be %.200s, not %.200s",
                     ObjectType.tp_name, target->ob_type->tp_name);
        return -1;
    }

    Py_XDECREF(self->target);
    self->target = target;
    Py_INCREF(target);
    git_tag_set_target(self->tag, target->obj);
    return 0;
}

static PyObject *
Tag_get_target_type(Tag *self) {
    if (!self->target)
        Py_RETURN_NONE;
    return PyInt_FromLong(git_tag_type(self->tag));
}

static PyObject *
Tag_get_name(Tag *self) {
    const char *name;
    name = git_tag_name(self->tag);
    if (!name)
        Py_RETURN_NONE;
    return PyString_FromString(name);
}

static int
Tag_set_name(Tag *self, PyObject *py_name) {
    char *name;
    if (!PyString_Check(py_name)) {
        PyErr_Format(PyExc_TypeError, "name must be str, not %.200s",
                     py_name->ob_type->tp_name);
        return -1;
    }
    name = PyString_AsString(py_name);
    if (!name)
        return -1;
    git_tag_set_name(self->tag, name);
    return 0;
}

static PyObject *
Tag_get_tagger(Tag *tag) {
    git_person *tagger;
    tagger = (git_person*)git_tag_tagger(tag->tag);
    if (!tagger)
        Py_RETURN_NONE;
    return Py_BuildValue("(ssl)", git_person_name(tagger),
                         git_person_email(tagger),
                         git_person_time(tagger));
}

static int
Tag_set_tagger(Tag *tag, PyObject *value) {
    char *name = NULL, *email = NULL;
    long long time;
    if (!PyArg_ParseTuple(value, "ssL", &name, &email, &time))
        return -1;
    git_tag_set_tagger(tag->tag, name, email, time);
    return 0;
}

static PyObject *
Tag_get_message(Tag *self) {
    const char *message;
    message = git_tag_message(self->tag);
    if (!message)
        Py_RETURN_NONE;
    return PyString_FromString(message);
}

static int
Tag_set_message(Tag *self, PyObject *message) {
    if (!PyString_Check(message)) {
        PyErr_Format(PyExc_TypeError, "message must be str, not %.200s",
                     message->ob_type->tp_name);
        return -1;
    }
    git_tag_set_message(self->tag, PyString_AS_STRING(message));
    return 0;
}

static PyGetSetDef Tag_getseters[] = {
    {"target", (getter)Tag_get_target, (setter)Tag_set_target, "tagged object",
     NULL},
    {"target_type", (getter)Tag_get_target_type, NULL, "type of tagged object",
     NULL},
    {"name", (getter)Tag_get_name, (setter)Tag_set_name, "tag name", NULL},
    {"tagger", (getter)Tag_get_tagger, (setter)Tag_set_tagger, "tagger", NULL},
    {"message", (getter)Tag_get_message, (setter)Tag_set_message, "tag message",
     NULL},
    {NULL}
};

static PyTypeObject TagType = {
    PyObject_HEAD_INIT(NULL)
    0,                                         /*ob_size*/
    "pygit2.Tag",                              /*tp_name*/
    sizeof(Tag),                               /*tp_basicsize*/
    0,                                         /*tp_itemsize*/
    (destructor)Tag_dealloc,                   /*tp_dealloc*/
    0,                                         /*tp_print*/
    0,                                         /*tp_getattr*/
    0,                                         /*tp_setattr*/
    0,                                         /*tp_compare*/
    0,                                         /*tp_repr*/
    0,                                         /*tp_as_number*/
    0,                                         /*tp_as_sequence*/
    0,                                         /*tp_as_mapping*/
    0,                                         /*tp_hash */
    0,                                         /*tp_call*/
    0,                                         /*tp_str*/
    0,                                         /*tp_getattro*/
    0,                                         /*tp_setattro*/
    0,                                         /*tp_as_buffer*/
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,  /*tp_flags*/
    "Tag objects",                             /* tp_doc */
    0,                                         /* tp_traverse */
    0,                                         /* tp_clear */
    0,                                         /* tp_richcompare */
    0,                                         /* tp_weaklistoffset */
    0,                                         /* tp_iter */
    0,                                         /* tp_iternext */
    0,                                         /* tp_methods */
    0,                                         /* tp_members */
    Tag_getseters,                             /* tp_getset */
    0,                                         /* tp_base */
    0,                                         /* tp_dict */
    0,                                         /* tp_descr_get */
    0,                                         /* tp_descr_set */
    0,                                         /* tp_dictoffset */
    (initproc)Tag_init,                        /* tp_init */
    0,                                         /* tp_alloc */
    0,                                         /* tp_new */
};

static PyMethodDef module_methods[] = {
    {NULL}
};

PyMODINIT_FUNC
initpygit2(void)
{
    PyObject* m;

    GitError = PyErr_NewException("pygit2.GitError", NULL, NULL);

    RepositoryType.tp_new = PyType_GenericNew;
    if (PyType_Ready(&RepositoryType) < 0)
        return;
    /* Do not set ObjectType.tp_new, to prevent creating Objects directly. */
    if (PyType_Ready(&ObjectType) < 0)
        return;
    CommitType.tp_base = &ObjectType;
    CommitType.tp_new = PyType_GenericNew;
    if (PyType_Ready(&CommitType) < 0)
        return;
    TreeEntryType.tp_base = &ObjectType;
    TreeEntryType.tp_new = PyType_GenericNew;
    if (PyType_Ready(&TreeEntryType) < 0)
        return;
    TreeType.tp_base = &ObjectType;
    TreeType.tp_new = PyType_GenericNew;
    if (PyType_Ready(&TreeType) < 0)
        return;
    BlobType.tp_base = &ObjectType;
    BlobType.tp_new = PyType_GenericNew;
    if (PyType_Ready(&BlobType) < 0)
        return;
    TagType.tp_base = &ObjectType;
    TagType.tp_new = PyType_GenericNew;
    if (PyType_Ready(&TagType) < 0)
        return;

    m = Py_InitModule3("pygit2", module_methods,
                       "Python bindings for libgit2.");

    if (m == NULL)
      return;

    Py_INCREF(GitError);
    PyModule_AddObject(m, "GitError", GitError);

    Py_INCREF(&RepositoryType);
    PyModule_AddObject(m, "Repository", (PyObject *)&RepositoryType);

    Py_INCREF(&ObjectType);
    PyModule_AddObject(m, "Object", (PyObject *)&ObjectType);

    Py_INCREF(&CommitType);
    PyModule_AddObject(m, "Commit", (PyObject *)&CommitType);

    Py_INCREF(&TreeEntryType);
    PyModule_AddObject(m, "TreeEntry", (PyObject *)&TreeEntryType);

    Py_INCREF(&TreeType);
    PyModule_AddObject(m, "Tree", (PyObject *)&TreeType);

    Py_INCREF(&BlobType);
    PyModule_AddObject(m, "Blob", (PyObject *)&BlobType);

    Py_INCREF(&TagType);
    PyModule_AddObject(m, "Tag", (PyObject *)&TagType);

    PyModule_AddIntConstant(m, "GIT_OBJ_ANY", GIT_OBJ_ANY);
    PyModule_AddIntConstant(m, "GIT_OBJ_COMMIT", GIT_OBJ_COMMIT);
    PyModule_AddIntConstant(m, "GIT_OBJ_TREE", GIT_OBJ_TREE);
    PyModule_AddIntConstant(m, "GIT_OBJ_BLOB", GIT_OBJ_BLOB);
    PyModule_AddIntConstant(m, "GIT_OBJ_TAG", GIT_OBJ_TAG);
}
