struct cs1550_sem {

	int value;
	struct pnode * head;

} typedef cs1550_sem;


// priority queue node struct
struct pnode {

	int priority;
	struct pnode * next;
	struct task_struct * task;

} typedef pnode;
