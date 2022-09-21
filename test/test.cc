#include <tcmalloc/tcmalloc.h>
// using namespace std;
int main()
{
    //std::cout<<__cplusplus<<std::endl;
     char *cp = (char *)TCMallocInternalMalloc(23 * sizeof(char));
    //TCMallocInternalFree(cp);
    //cp = NULL;
    return 0;
}
