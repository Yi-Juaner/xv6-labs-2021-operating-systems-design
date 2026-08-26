#include "kernel/types.h"
#include "kernel/param.h"
#include "user/user.h"

#define LINE_SIZE 512

static void run_line(char *line, int length, int argc, char *argv[])
{
  char *args[MAXARG];
  char *p;
  int n;
  int pid;

  n = 0;
  for(int i = 1; i < argc; i++)
    args[n++] = argv[i];

  line[length] = 0;
  p = line;
  while(*p != 0)
  {
    while(*p == ' ' || *p == '\t' || *p == '\r')
      p++;
    if(*p == 0)
      break;

    if(n >= MAXARG - 1)
    {
      fprintf(2, "xargs: too many arguments\n");
      exit(1);
    }
    args[n++] = p;

    while(*p != 0 && *p != ' ' && *p != '\t' && *p != '\r')
      p++;
    if(*p != 0)
      *p++ = 0;
  }
  args[n] = 0;

  pid = fork();
  if(pid < 0)
  {
    fprintf(2, "xargs: fork failed\n");
    exit(1);
  }

  if(pid == 0)
  {
    exec(args[0], args);
    fprintf(2, "xargs: exec %s failed\n", args[0]);
    exit(1);
  }

  wait(0);
}

int main(int argc, char *argv[])
{
  char line[LINE_SIZE];
  char c;
  int length;
  int result;

  if(argc < 2)
  {
    fprintf(2, "usage: xargs command [initial-args ...]\n");
    exit(1);
  }
  if(argc - 1 >= MAXARG)
  {
    fprintf(2, "xargs: too many initial arguments\n");
    exit(1);
  }

  length = 0;
  while((result = read(0, &c, 1)) > 0)
  {
    if(c == '\n')
    {
      run_line(line, length, argc, argv);
      length = 0;
      continue;
    }

    if(length >= LINE_SIZE - 1)
    {
      fprintf(2, "xargs: input line too long\n");
      exit(1);
    }
    line[length++] = c;
  }

  if(result < 0)
  {
    fprintf(2, "xargs: read failed\n");
    exit(1);
  }
  if(length > 0)
    run_line(line, length, argc, argv);

  exit(0);
}