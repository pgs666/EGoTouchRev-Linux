#include <stdio.h>

void run_detection_tests(void);
void run_runtime_tests(void);
void run_tracking_tests(void);

int main(void)
{
	run_detection_tests();
	run_runtime_tests();
	run_tracking_tests();
	puts("hx-algo host regression tests passed");
	return 0;
}
