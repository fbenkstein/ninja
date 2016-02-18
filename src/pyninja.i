%module(directors="1") pyninja

// Support automatic mapping from string to Python str.
%include "std_string.i"
// exception handling
%include "exception.i"

%{
#include "manifest_parser.h"
#include "state.h"
#include "util.h"
#include "build.h"
#include "version.h"
#include "build_log.h"
#include "deps_log.h"
#include "build.h"
#include "disk_interface.h"
#include "metrics.h"
%}

typedef std::string string;

// Provide a custom exception class.  Rather than creating this class
// in C++ it is easier to make this a pure python class and add it
// during the initialization of the wrapper module.
%{
static PyObject *NinjaError = NULL;
%}

%inline %{
void _set_ninja_error_class(PyObject *exc) {
    Py_XDECREF(NinjaError);
    Py_XINCREF(exc);
    NinjaError = exc;
}
%}

%pythoncode %{
class NinjaError(Exception):
    pass

_set_ninja_error_class(NinjaError)
del _set_ninja_error_class
%}

// Propagate exceptions from virtual methods.
%exception {
    try {
        $action;
    }
    catch (const Swig::DirectorException &) {
        SWIG_fail;
    }
    // Catch std::exceptions and translate them to Python.
    SWIG_CATCH_STDEXCEPT
}

%feature("director:except") %{
     (void)$error;
     throw Swig::DirectorMethodException();
%}

// These typedefs allow us to transform ninja's error handling
// consisting of passing around a string pointer and checking for
// boolean success into Python exceptions.

%inline %{
// Some methods return false and set the error message on error.
typedef bool success_and_message_t;
typedef string* error_message_t;
%}

%typemap(out) success_and_message_t %{
    if ($1) {
        Py_INCREF(Py_None);
        $result = Py_None;
    }
%}

%typemap(in, numinputs=0) error_message_t (string temp) %{
    $1 = &temp;
%}

%typemap(argout) error_message_t %{
    if (!$result) {
        PyErr_SetString(NinjaError, $1->c_str());
        Py_CLEAR($result);
        SWIG_fail;
    }
%}

%inline %{
// Other methods only set the error message and return some other
// type.
typedef string* error_message_empty_t;
%}

%typemap(in, numinputs=0) error_message_empty_t (string temp) %{
    $1 = &temp;
%}

%typemap(argout) error_message_empty_t %{
    if (!$1->empty()) {
        PyErr_SetString(NinjaError, $1->c_str());
        Py_CLEAR($result);
        SWIG_fail;
    }
%}


%inline %{
// There are also methods that return a boolean but false is not an
// error when the error message is empty.
typedef bool boolean_and_message_t;
typedef string* boolean_error_message_t;
%}

%typemap(out) boolean_and_message_t %{
    $result = PyBool_FromLong($1);
%}

%typemap(in, numinputs=0) boolean_error_message_t (string temp) %{
    $1 = &temp;
%}

%typemap(argout) boolean_error_message_t %{
    if (!($result && PyObject_IsTrue($result)) && !$1->empty()) {
        PyErr_SetString(NinjaError, $1->c_str());
        Py_CLEAR($result);
        SWIG_fail;
    }
%}

%typemap(in) StringPiece (std::string s) {
    if (!PyString_Check($input)) {
        PyErr_SetString(PyExc_TypeError, "expected a string");
        SWIG_fail;
    }

    s.assign(PyString_AS_STRING($input), PyString_GET_SIZE($input));
    $1 = s;
}

%typemap(directorin) StringPiece %{
    $input = PyString_FromStringAndSize($1.str_, $1.len_);

    if (!$input) {
        throw Swig::DirectorMethodException();
    }
%}

// Expose version number to Python.
%rename(version) kNinjaVersion;
%constant const char *kNinjaVersion;

struct BindingEnv {
    string LookupVariable(const string&);

private:
    BindingEnv();
};

struct BuildConfig {
  enum Verbosity {
    NORMAL,
    QUIET,  // No output -- used when testing.
    VERBOSE
  };
  Verbosity verbosity;
  bool dry_run;
  int parallelism;
  int failures_allowed;
  double max_load_average;
};

// Vector helper for vector<Node*> and vector<Edge*>.
%define VectorHelper(Item)
%{
typedef vector<Item *> Item ## Vector;
%}

%feature("python:slot", "sq_length", functype="lenfunc") Item ## Vector::size;
%feature("python:slot", "sq_item", functype="ssizeargfunc") Item ## Vector::get_item;
struct Item ## Vector {
    size_t size() const;

    %extend {
        Item *get_item(long n) {
            if (n < 0) {
                n = $self->size() - n;
            }

            return $self->at(n);
        }
    }

private:
    Item ## Vector();
};
%enddef

struct Node;
struct Edge;

VectorHelper(Node);
VectorHelper(Edge);

struct Node {
    Edge* in_edge();

private:
    Node();
};

struct Edge {

private:
    Edge();
};


struct State {
    BindingEnv bindings_;

    Node* LookupNode(StringPiece path) const;
    NodeVector DefaultNodes(error_message_empty_t err);
};

%feature(director) BuildLogUser;
struct BuildLogUser {
    virtual ~BuildLogUser();
    virtual bool IsPathDead(StringPiece s) const = 0;
};

struct BuildLog {
    success_and_message_t Load(const string& path, error_message_t err);
    success_and_message_t OpenForWrite(const string& path, const BuildLogUser& user, error_message_t err);
};

struct DepsLog {
    success_and_message_t Load(const string& path, State* state, error_message_t err);
    success_and_message_t OpenForWrite(const string& path, error_message_t err);
};

%inline %{
// Return a tuple of a new string containing the canonicalized path
// and the slash bits instead of modifying the string in-place.
PyObject *CanonicalizePath(const string &path) {
    string err;
    string s2(path);
    unsigned int slash_bits;

    if (!CanonicalizePath(&s2, &slash_bits, &err)) {
        PyErr_SetString(NinjaError, err.c_str());
        return NULL;
    }

    return Py_BuildValue("(s#I)", s2.c_str(), s2.size(), slash_bits);
}
%}

struct FileReader {
    enum Status {
        Okay,
        NotFound,
        OtherError
    };

    virtual ~FileReader() {}
    virtual Status ReadFile(const string& path, string* contents, error_message_t err) = 0;
};

typedef int TimeStamp;

struct DiskInterface : FileReader {
    virtual ~DiskInterface() {}
    virtual TimeStamp Stat(const string& path, error_message_empty_t err) const = 0;
};

struct RealDiskInterface : public DiskInterface {
    virtual TimeStamp Stat(const string& path, error_message_empty_t err) const;
    virtual Status ReadFile(const string& path, string* contents, error_message_t err);
};

%{
DiskInterface *get_RealDiskInterface() {
    static RealDiskInterface disk_interface;
    return &disk_interface;
}
%}

enum DupeEdgeAction {
  kDupeEdgeActionWarn,
  kDupeEdgeActionError,
};

struct ManifestParser {
    ManifestParser(State* state, FileReader* file_reader,
                   DupeEdgeAction dupe_edge_action);
    success_and_message_t Load(const string& filename, error_message_t err);
};

%typemap(check) Node* %{
    if (!$1) {
        SWIG_exception_fail(SWIG_ValueError, "None not allowed");
    }
%}

%typemap(default) DiskInterface * {
    $1 = get_RealDiskInterface();
}

struct Builder {
    Builder(State* state, const BuildConfig& config,
            BuildLog* build_log, DepsLog* deps_log,
            DiskInterface* disk_interface);
    ~Builder();

    bool AlreadyUpToDate();
    boolean_and_message_t AddTarget(Node* target, boolean_error_message_t err);
    success_and_message_t Build(error_message_t err);
};

// Switch on newer language features in the generated Python module.
%pythonbegin %{
from __future__ import print_function, division

def _enable_run_from_src():
    'Enable running from the source directory.'
    import sys, os
    path = os.path.dirname(os.path.abspath(__file__))
    if os.path.basename(path) == 'src':
        sys.path.append(os.path.join(path, '..', 'build'))
_enable_run_from_src()
del _enable_run_from_src
%}

// Include custom Python code into the wrapper module.
%pythoncode "pyninja.in.py"
