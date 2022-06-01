namespace cpp echo
namespace py echo

service Echo
{
  oneway void echo(1: string arg);
  oneway void test(1: i32 time);
}


