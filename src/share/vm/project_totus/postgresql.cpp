#include "postgresql.hpp"

#include "runtime/os.hpp"

#undef max
#undef min

#include <list>
#include <sstream>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <arpa/inet.h>
#include <libpq-fe.h>

using namespace project_totus;

namespace project_totus {

PostgreSQL *postgresql = nullptr;

struct PostgreSQLImpl {
  PGconn *Connection;
  std::unordered_map<std::string, uint32_t> MethodIDMap;
  std::unordered_set<std::string> InlineMethodCall;
};

}

namespace {

pthread_mutex_t exec_mutex = PTHREAD_MUTEX_INITIALIZER;

class Params {
  std::vector<const char *> Values;
  std::list<uint32_t> BinaryValues; // Required for stable iterators
  std::vector<int> Lengths;
  std::vector<int> Formats;
public:
  void addText(const char *Text) {
    Values.push_back(Text);
    Lengths.push_back(0); // Ignored for text, used for binary
    Formats.push_back(0); // 0 is text, 1 is binary
  }
  void addBinary(uint32_t Binary) {
    BinaryValues.push_back(htonl(Binary));
    auto &BinaryValue = BinaryValues.back();
    const char *Value = (const char *) &BinaryValue;

    Values.push_back(Value);
    Lengths.push_back(sizeof(BinaryValue));
    Formats.push_back(1); // 1 is binary
  }
  void addBool(bool B) {
    if (B)
      addText("true");
    else
      addText("false");
  }
  void clear() {
    Values.clear();
    BinaryValues.clear();
    Lengths.clear();
    Formats.clear();
  }

  const char * const * getValues() const {
    return Values.data();
  }
  const int * getLengths() const {
    return Lengths.data();
  }
  const int * getFormats() const {
    return Formats.data();
  }
  int getN() const {
    if (Values.size() != Lengths.size()) {
      printf("ERROR: database parameters mismatched\n");
      os::abort();
    }
    if (Values.size() != Formats.size()) {
      printf("ERROR: database parameters mismatched\n");
      os::abort();
    }
    return Values.size();
  }
};

PGresult * Exec(PostgreSQLImpl &Impl,
		const char *Query,
		const Params &P) {
  pthread_mutex_lock(&exec_mutex);
  auto Result = PQexecParams(Impl.Connection, Query, P.getN(), nullptr,
			     P.getValues(), P.getLengths(), P.getFormats(), 1);
  pthread_mutex_unlock(&exec_mutex);
  return Result;
}

void ExecCommand(PostgreSQLImpl &Impl,
		 const char *Query,
		 const Params &Params) {
  auto Result = Exec(Impl, Query, Params);
  auto ResultStatus = PQresultStatus(Result);
  if (ResultStatus != PGRES_COMMAND_OK) {
    printf("ERROR: database did not execute command: %s\n",
	   PQresultErrorMessage(Result));
    os::abort();
  }
  PQclear(Result);
}

PGresult * ExecTuples(PostgreSQLImpl &Impl,
		      const char *Query,
		      const Params &Params) {
  auto Result = Exec(Impl, Query, Params);
  auto ResultStatus = PQresultStatus(Result);
  if (ResultStatus != PGRES_TUPLES_OK) {
    printf("ERROR: database did not return valid tuples: %s\n",
	   PQresultErrorMessage(Result));
    os::abort();
  }
  return Result;
}

uint32_t GetID(PostgreSQLImpl &Impl,
	       const char *Query,
	       const Params &Params) {
  auto Result = ExecTuples(Impl, Query, Params);
  if (PQntuples(Result) != 1) {
    printf("ERROR: database returned more than one ID\n");
    os::abort();
  }
  int FieldIndex = PQfnumber(Result, "id");
  char *Value = PQgetvalue(Result, 0, FieldIndex);
  uint32_t ID = ntohl(*((uint32_t *) Value));
  PQclear(Result);
  return ID;
}

}

PostgreSQL::PostgreSQL(const std::string &PackageName,
		       const std::string &PackageVersion,
		       const std::string &ExperimentName,
		       const char *InlineSetName)
  : Impl(new PostgreSQLImpl), InlineSetID(0) {
  Impl->Connection = PQconnectdb("dbname=project_totus");
  if (PQstatus(Impl->Connection) != CONNECTION_OK) {
    printf("ERROR: can not connect to database\n");
    os::abort();
  }
  Params Params;
  Params.addText("project_totus");
  Params.addText("0007_increase_klass_max_length");
  auto Result = ExecTuples(
    *Impl,
    "SELECT id FROM django_migrations WHERE app = $1 AND name = $2;",
    Params);
  if (PQntuples(Result) != 1) {
    printf("ERROR: apply the latest database migration\n");
    os::abort();
  }
  PQclear(Result);
  Params.clear();

  Params.addText(PackageName.c_str());
  ExecCommand(
    *Impl,
    "INSERT INTO project_totus_package_base (name) VALUES ($1)"
    " ON CONFLICT DO NOTHING",
    Params);
  uint32_t PackageNameID = GetID(
    *Impl,
    "SELECT id FROM project_totus_package_base WHERE name = $1",
    Params);
  Params.clear();

  Params.addBinary(PackageNameID);
  Params.addText(PackageVersion.c_str());
  ExecCommand(
    *Impl,
    "INSERT INTO project_totus_package (base_id, version) VALUES ($1, $2)"
    " ON CONFLICT DO NOTHING",
    Params);
  PackageID = GetID(
    *Impl,
    "SELECT id FROM project_totus_package"
    " WHERE base_id = $1 AND version = $2",
    Params);
  Params.clear();

  Params.addBinary(PackageID);
  Params.addText(ExperimentName.c_str());
  ExecCommand(
    *Impl,
    "INSERT INTO project_totus_experiment (package_id, name) VALUES ($1, $2)"
    " ON CONFLICT DO NOTHING",
    Params);
  ExperimentID = GetID(
    *Impl,
    "SELECT id FROM project_totus_experiment"
    " WHERE package_id = $1 AND name = $2",
    Params);
  Params.clear();

  if (InlineSetName != nullptr) {
    Params.addBinary(ExperimentID);
    Params.addText(InlineSetName);
    InlineSetID = GetID(
      *Impl,
      "SELECT id FROM project_totus_inline_set"
      " WHERE experiment_id = $1 AND name = $2",
      Params);
    Params.clear();

    Params.addBinary(InlineSetID);
    const char *Query = "SELECT"
      " \"project_totus_klass\".\"name\","
      " \"project_totus_method\".\"name\","
      " \"project_totus_method\".\"descriptor\","
      " \"project_totus_call_site\".\"bci\","
      " T9.\"name\","
      " T8.\"name\","
      " T8.\"descriptor\""
      " FROM \"project_totus_inline_method_call\""
      " INNER JOIN"
      " \"project_totus_inline_set_method_call\""
      " ON (\"project_totus_inline_method_call\".\"id\" = \"project_totus_inline_set_method_call\".\"inline_method_call_id\")"
      " INNER JOIN \"project_totus_method_call\""
      " ON (\"project_totus_inline_method_call\".\"method_call_id\" = \"project_totus_method_call\".\"id\")"
      " INNER JOIN \"project_totus_call_site\""
      " ON (\"project_totus_method_call\".\"call_site_id\" = \"project_totus_call_site\".\"id\")"
      " INNER JOIN \"project_totus_method\""
      " ON (\"project_totus_call_site\".\"caller_id\" = \"project_totus_method\".\"id\")"
      " INNER JOIN \"project_totus_klass\""
      " ON (\"project_totus_method\".\"klass_id\" = \"project_totus_klass\".\"id\")"
      " INNER JOIN \"project_totus_method\" T8"
      " ON (\"project_totus_method_call\".\"callee_id\" = T8.\"id\")"
      " INNER JOIN \"project_totus_klass\" T9"
      " ON (T8.\"klass_id\" = T9.\"id\")"
      " WHERE \"project_totus_inline_set_method_call\".\"inline_set_id\" = $1"
      " ORDER BY \"project_totus_method\".\"name\""
      " ASC, \"project_totus_call_site\".\"bci\" ASC, T8.\"name\" ASC";
    Result = ExecTuples(
      *Impl,
      Query,
      Params);
    for (int i = 0; i < PQntuples(Result); ++i) {
      const char *caller_klass_name = PQgetvalue(Result, i, 0);
      const char *caller_method_name = PQgetvalue(Result, i, 1);
      const char *caller_method_descriptor = PQgetvalue(Result, i, 2);
      uint32_t bci = ntohl(*((uint32_t *)PQgetvalue(Result, i, 3)));
      const char *callee_klass_name = PQgetvalue(Result, i, 4);
      const char *callee_method_name = PQgetvalue(Result, i, 5);
      const char *callee_method_descriptor = PQgetvalue(Result, i, 6);

      std::stringstream ss;
      ss << caller_klass_name
         << '.' << caller_method_name << caller_method_descriptor
         << '@' << bci << ' '
         << callee_klass_name
         << '.' << callee_method_name << callee_method_descriptor;

      // if (strcmp(caller_klass_name, DEBUG_CALLER_KLASS_NAME) == 0
      // 	  && bci == DEBUG_CALLER_BCI) {
      // 	printf("PostgreSQL:InlineMethodCall.insert: %s\n", ss.str().c_str());
      // }
      Impl->InlineMethodCall.insert(ss.str());
    }
    PQclear(Result);
    Params.clear();
  }
}

PostgreSQL::~PostgreSQL() {
  PQfinish(Impl->Connection);
}

uint32_t PostgreSQL::getMethodID(ciMethod * method)
{
  std::stringstream ss;
  ss << method->holder()->name()->as_utf8()
     << '.' << method->name()->as_utf8()
     << method->signature()->as_symbol()->as_utf8();
  std::string full_method_name = ss.str();

  if (Impl->MethodIDMap.count(full_method_name) == 0) {
    Params Params;
    Params.addBinary(PackageID);
    Params.addText(method->holder()->name()->as_utf8());
    ExecCommand(
      *Impl,
      "INSERT INTO project_totus_klass (package_id, name) VALUES ($1, $2)"
      " ON CONFLICT DO NOTHING",
      Params);
    uint32_t KlassID = GetID(
      *Impl,
      "SELECT id FROM project_totus_klass"
      " WHERE package_id = $1 AND name = $2",
      Params);
    Params.clear();
    Params.addBinary(KlassID);
    Params.addText(method->name()->as_utf8());
    Params.addText(method->signature()->as_symbol()->as_utf8());
    Params.addBool(!method->is_static());
    Params.addBinary(method->code_size());
    ExecCommand(
      *Impl,
      "INSERT INTO project_totus_method"
      " (klass_id, name, descriptor, is_instance_method, size) VALUES ($1, $2, $3, $4, $5)"
      " ON CONFLICT DO NOTHING",
      Params);
    Params.clear();

    Params.addBinary(KlassID);
    Params.addText(method->name()->as_utf8());
    Params.addText(method->signature()->as_symbol()->as_utf8());
    uint32_t MethodID = GetID(
      *Impl,
      "SELECT id FROM project_totus_method"
      " WHERE klass_id = $1 AND name = $2 AND descriptor = $3",
      Params);
    Params.clear();
    Impl->MethodIDMap[full_method_name] = MethodID;
  }
  return Impl->MethodIDMap[full_method_name];
}

uint32_t PostgreSQL::getCallSiteID(ciMethod * caller, int bci)
{

  uint32_t CallerID = getMethodID(caller);

  // if (strcmp(caller->holder()->name()->as_utf8(), DEBUG_CALLER_KLASS_NAME) == 0
  //     && bci == DEBUG_CALLER_BCI) {
  //   printf("  getCallSiteID: %s.%s%s@%d return CallerID=%d\n",
  // 	   caller->holder()->name()->as_utf8(),
  // 	   caller->name()->as_utf8(),
  // 	   caller->signature()->as_symbol()->as_utf8(),
  // 	   bci,
  // 	   CallerID);
  // }
  Params Params;
  Params.addBinary(CallerID);
  Params.addBinary(bci);
  ExecCommand(
    *Impl,
    "INSERT INTO project_totus_call_site (caller_id, bci) VALUES ($1, $2)"
    " ON CONFLICT DO NOTHING",
    Params);
  uint32_t CallSiteID = GetID(
    *Impl,
    "SELECT id FROM project_totus_call_site"
    " WHERE caller_id = $1 AND bci = $2",
    Params);
  Params.clear();
  return CallSiteID;
}

uint32_t PostgreSQL::getMethodCallID(uint32_t CallSiteID, uint32_t CalleeID)
{
  Params Params;
  Params.addBinary(CallSiteID);
  Params.addBinary(CalleeID);
  ExecCommand(
    *Impl,
    "INSERT INTO project_totus_method_call (call_site_id, callee_id) VALUES ($1, $2)"
    " ON CONFLICT DO NOTHING",
    Params);
  uint32_t MethodCallID = GetID(
    *Impl,
    "SELECT id FROM project_totus_method_call"
    " WHERE call_site_id = $1 AND callee_id = $2",
    Params);
  Params.clear();
  return MethodCallID;
}

uint32_t PostgreSQL::getInlineMethodCallID(uint32_t MethodCallID)
{
  Params Params;
  Params.addBinary(ExperimentID);
  Params.addBinary(MethodCallID);
  ExecCommand(
    *Impl,
    "INSERT INTO project_totus_inline_method_call (experiment_id, method_call_id) VALUES ($1, $2)"
    " ON CONFLICT DO NOTHING",
    Params);
  uint32_t InlineMethodCallID = GetID(
    *Impl,
    "SELECT id FROM project_totus_inline_method_call"
    " WHERE experiment_id = $1 AND method_call_id = $2",
    Params);
  Params.clear();
  return InlineMethodCallID;
}

bool PostgreSQL::forceInline(ciMethod *caller,
			     int bci,
			     ciMethod * callee)
{
  std::stringstream ss;
  ss << caller->holder()->name()->as_utf8()
     << '.' << caller->name()->as_utf8()
     << caller->signature()->as_symbol()->as_utf8()
     << '@' << bci << ' '
     << callee->holder()->name()->as_utf8()
     << '.' << callee->name()->as_utf8()
     << callee->signature()->as_symbol()->as_utf8();

  // if (strcmp(caller->holder()->name()->as_utf8(), DEBUG_CALLER_KLASS_NAME) == 0
  //     && bci == DEBUG_CALLER_BCI) {
  //   printf("forceInline: %s.%s%s@%d %s.%s%s return %d\n",
  // 	   caller->holder()->name()->as_utf8(),
  // 	   caller->name()->as_utf8(),
  // 	   caller->signature()->as_symbol()->as_utf8(),
  // 	   bci,
  // 	   callee->holder()->name()->as_utf8(),
  // 	   callee->name()->as_utf8(),
  // 	   callee->signature()->as_symbol()->as_utf8(),
  // 	   Impl->InlineMethodCall.count(ss.str()));
  // }

  return Impl->InlineMethodCall.count(ss.str()) > 0;
}

uint32_t PostgreSQL::getInlineMethodCallID(ciMethod *caller,
					   int bci,
					   ciMethod * callee)
{
  // if (strcmp(caller->holder()->name()->as_utf8(), DEBUG_CALLER_KLASS_NAME) == 0
  //     && bci == DEBUG_CALLER_BCI) {
  //   printf("getInlineMethodCallID: %s.%s%s@%d %s.%s%s\n",
  // 	   caller->holder()->name()->as_utf8(),
  // 	   caller->name()->as_utf8(),
  // 	   caller->signature()->as_symbol()->as_utf8(),
  // 	   bci,
  // 	   callee->holder()->name()->as_utf8(),
  // 	   callee->name()->as_utf8(),
  // 	   callee->signature()->as_symbol()->as_utf8());
  // }

  uint32_t CallSiteID = getCallSiteID(caller, bci);
  uint32_t CalleeID = getMethodID(callee);
  uint32_t MethodCallID = getMethodCallID(CallSiteID, CalleeID);
  return getInlineMethodCallID(MethodCallID);
}

void PostgreSQL::addInlineDecision(uint32_t inline_method_call_id,
				   bool require_inline)
{

  Params Params;
  Params.addBinary(inline_method_call_id);
  Params.addBool(require_inline);
  ExecCommand(
    *Impl,
    "INSERT INTO project_totus_inline_decision (inline_method_call_id, require_inline) VALUES ($1, $2)",
    Params);
  Params.clear();
}
