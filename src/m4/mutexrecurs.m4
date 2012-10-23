
AC_DEFUN([AC_MUTEX_RECURS],
[
    AC_MSG_CHECKING([PTHREAD_MUTEX_RECURSIVE])
    AC_CACHE_VAL(ac_cv_attr_mutex_recurs, [
      AC_TRY_COMPILE([
#include <stdlib.h>
#include <pthread.h>
], [
    pthread_mutexattr_t v;
    (void) pthread_mutexattr_settype(&v, PTHREAD_MUTEX_RECURSIVE_NP);
], ac_cv_attr_mutex_recurs_arg1="PTHREAD_MUTEX_RECURSIVE_NP", ac_cv_attr_mutex_recurs_arg1="PTHREAD_MUTEX_RECURSIVE")
      ac_cv_attr_mutex_recurs="$ac_cv_attr_mutex_recurs_arg1"])
    AC_MSG_RESULT([$]{ac_t:-}[$]ac_cv_attr_mutex_recurs)
    AC_DEFINE_UNQUOTED(PTHREAD_MUTEX_RECURSIVE_WITH_FEELING, $ac_cv_attr_mutex_recurs_arg1,
      [Define param constant for pthread_mutexattr_settype.])
])


