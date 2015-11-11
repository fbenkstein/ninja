%module(directors="1") pyninja

// Support automatic mapping from string to Python str.
%include "std_string.i"
// For int64_t etc.
%include "stdint.i"
// exception handling
%include "exception.i"
// symbol constants for warning ids
%include "swigwarn.swg"

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

int64_t GetTimeMillis();

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
  double max_memory_usage;
};

struct Pool {
    explicit Pool(const string& name, int depth);
    bool is_valid() const;
    int depth() const;
    const string& name() const;
    bool ShouldDelayEdge() const;
    void EdgeScheduled(const Edge& edge);
    void EdgeFinished(const Edge& edge);
    void DelayEdge(Edge* edge);
    // TODO: void RetrieveReadyEdges(set<Edge*>* ready_queue);
    void Dump() const;
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

%warnfilter(SWIGWARN_PARSE_BUILTIN_NAME) Node::id;

struct Node {
    const string& path() const;
    string PathDecanonicalized() const;
    bool dirty() const;
    void set_dirty(bool dirty);
    void MarkDirty();
    Edge* in_edge();
    int id() const;
    void set_id(int id);
    const EdgeVector &out_edges();
    void AddOutEdge(Edge* edge);
    void Dump(const char* prefix="");

private:
    Node();
};

struct Rule {
    explicit Rule(const string& name);
    const string& name() const { return name_; }
    // TODO: typedef map<string, EvalString> Bindings;
    void AddBinding(const string& key, const EvalString& val);
    static bool IsReservedBinding(const string& var);
    // TODO: const EvalString* GetBinding(const string& key) const;
private:
    Rule();
};

%warnfilter(SWIGWARN_PARSE_BUILTIN_NAME) Edge::hash;

%feature("python:slot", "tp_hash", functype="hashfunc") Edge::hash;
struct Edge {
    bool AllInputsReady() const;
    string EvaluateCommand(bool incl_rsp_file = false);
    string GetBinding(const string& key);
    bool GetBindingBool(const string& key);
    string GetUnescapedDepfile();
    string GetUnescapedRspfile();
    void Dump(const char* prefix="") const;
    const Rule* rule_;
    Pool* pool_;
    NodeVector inputs_;
    NodeVector outputs_;
    BindingEnv* env_;
    bool outputs_ready_;
    bool deps_missing_;
    const Rule& rule() const;
    Pool* pool() const;
    int weight() const;
    bool outputs_ready();
    int implicit_deps_;
    int order_only_deps_;
    bool is_implicit(size_t index);
    bool is_order_only(size_t index);
    bool is_phony() const;
    bool use_console() const;

    %extend {
        long hash() const {
            return reinterpret_cast<long>($self);
        }
    }
private:
    Edge();
};


struct State {
    static Pool kDefaultPool;
    static Pool kConsolePool;
    static const Rule kPhonyRule;

    void AddRule(const Rule* rule);
    void AddPool(Pool* pool);
    Edge* AddEdge(const Rule* rule);

    EdgeVector edges_;
    BindingEnv bindings_;
    NodeVector defaults_;

    Pool* LookupPool(const string& pool_name);
    Node* LookupNode(StringPiece path) const;
    const Rule* LookupRule(const string& rule_name);

    NodeVector RootNodes(error_message_empty_t err);
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

%{
typedef ManifestParser::FileReader FileReader;
%}

struct FileReader {
    virtual ~FileReader() {}
    virtual bool ReadFile(const string& path, string* content, string* err) = 0;
};

%{
struct RealFileReader : public FileReader {
  virtual bool ReadFile(const string& path, string* content, string* err) {
    return ::ReadFile(path, content, err) == 0;
  }
};
%}

struct RealFileReader : FileReader {
    virtual bool ReadFile(const string& path, string* content, string* err);
};

%{
FileReader *get_RealFileReader() {
    static RealFileReader file_reader;
    return &file_reader;
}
%}

%typemap(default) FileReader *file_reader {
    $1 = get_RealFileReader();
}

struct ManifestParser {
    ManifestParser(State* state, FileReader* file_reader);
    success_and_message_t Load(const string& filename, error_message_t err);
};

typedef int TimeStamp;

struct DiskInterface {
    virtual ~DiskInterface() {}
    virtual TimeStamp Stat(const string& path) const = 0;
};

struct RealDiskInterface : public DiskInterface {
    virtual TimeStamp Stat(const string& path) const;
};

%{
DiskInterface *get_RealDiskInterface() {
    static RealDiskInterface disk_interface;
    return &disk_interface;
}
%}

%typemap(check) Node* %{
    if (!$1) {
        SWIG_exception_fail(SWIG_ValueError, "None not allowed");
    }
%}

%typemap(default) DiskInterface * {
    $1 = get_RealDiskInterface();
}

struct BuildStatusInterface;

%typemap(memberin) BuildStatusInterface *status_ %{
    if ($1) {
        delete $1;
    }

    $1 = $input;
%}

struct Builder {
    Builder(State* state, const BuildConfig& config,
            BuildLog* build_log, DepsLog* deps_log,
            DiskInterface* disk_interface);
    ~Builder();

    bool AlreadyUpToDate();
    boolean_and_message_t AddTarget(Node* target, boolean_error_message_t err);
    success_and_message_t Build(error_message_t err);

    BuildStatusInterface* status_;
};

%typemap(in, numinputs=0) (int *start_time, int *end_time) (int temp_start_time, int temp_end_time) %{
    $1 = &temp_start_time;
    $2 = &temp_end_time;
%}

%typemap(argout) (int *start_time, int *end_time) %{
    Py_CLEAR($result);
    $result = Py_BuildValue("(ii)", *$1, *$2);
%}

%typemap(directorargout) (int *start_time, int *end_time) %{
    if ($result) {
        if (!PyArg_ParseTuple($result, "II", $1, $2)) {
            throw Swig::DirectorMethodException();
        }
    }
%}

%feature(director) BuildStatusInterface;
struct BuildStatusInterface {
    explicit BuildStatusInterface(const BuildConfig& config);
    virtual ~BuildStatusInterface() {}
    virtual void PlanHasTotalEdges(int total) = 0;
    virtual void BuildEdgeStarted(Edge* edge) = 0;
    virtual void BuildEdgeFinished(Edge* edge, bool success, const string& output,
                                   int* start_time, int* end_time) = 0;
    virtual void BuildFinished() = 0;
    virtual string FormatProgressStatus(const char* progress_status_format) const = 0;

protected:
    const BuildConfig& config_;
};

struct BuildStatus : BuildStatusInterface {
    explicit BuildStatus(const BuildConfig& config);
    virtual void PlanHasTotalEdges(int total);
    virtual void BuildEdgeStarted(Edge* edge);
    virtual void BuildEdgeFinished(Edge* edge, bool success, const string& output,
                                   int* start_time, int* end_time);
    virtual void BuildFinished();
    virtual string FormatProgressStatus(const char* progress_status_format) const;
};

// Switch on newer language features in the generated Python module.
%pythonbegin %{
from __future__ import print_function, division
%}

// Include custom Python code into the wrapper module.
%pythoncode "pyninja.in.py"
