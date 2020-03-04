#ifndef _SEM_H_
#define _SEM_H_

struct cs1550_sem {
  int value;
  struct my_queue* head;
  struct my_queue* tail;
} typedef cs1550_sem;

#endif
