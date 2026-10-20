/* Test the kernel's cmac(aes) via AF_ALG on the target device. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <linux/if_alg.h>

int main(void)
{
	struct sockaddr_alg sa;
	unsigned char key[16], msg[64], out[64];
	int s, op, i, n;

	memset(&sa, 0, sizeof(sa));
	sa.salg_family = AF_ALG;
	strcpy((char *)sa.salg_type, "cmac");
	strcpy((char *)sa.salg_name, "aes");

	s = socket(AF_ALG, SOCK_SEQPACKET, 0);
	if (s < 0) { perror("socket"); return 1; }
	if (bind(s, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		perror("bind");
		return 1;
	}
	memset(key, 0x55, sizeof(key));
	if (setsockopt(s, SOL_ALG, ALG_SET_KEY, key, sizeof(key)) < 0) {
		perror("setsockopt");
		return 1;
	}
	op = accept(s, NULL, NULL);
	if (op < 0) { perror("accept"); return 1; }

	/* msg = 02 || body, SN=TESTTEST, delay=0 */
	memset(msg, 0, sizeof(msg));
	msg[0] = 0x02; msg[1] = 0x03; msg[2] = 0xff; msg[3] = 0x01; msg[4] = 0x00;
	memcpy(msg + 5, "TESTTEST", 8);
	if (write(op, msg, 41) != 41) { perror("write"); return 1; }
	n = read(op, out, sizeof(out));
	printf("AF_ALG cmac(aes) len=%d:", n);
	for (i = 0; i < n && i < 16; i++)
		printf(" %02x", out[i]);
	printf("\n");
	printf("expect first8: cf280560e1a0d3cf (standard), kernel driver: d72464c876d7bc0c\n");
	return 0;
}
