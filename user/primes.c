#include "kernel/types.h"
#include "user/user.h"

static void sieve(int input_fd)
{
  int prime;
  int number;
  int next_pipe[2];
  int pid;

  if(read(input_fd, &prime, sizeof(prime)) != sizeof(prime))
  {
    close(input_fd);
    exit(0);
  }

  printf("prime %d\n", prime);

  if(pipe(next_pipe) < 0)
  {
    fprintf(2, "primes: pipe failed\n");
    close(input_fd);
    exit(1);
  }

  pid = fork();
  if(pid < 0)
  {
    fprintf(2, "primes: fork failed\n");
    close(input_fd);
    close(next_pipe[0]);
    close(next_pipe[1]);
    exit(1);
  }

  if(pid == 0)
  {
    close(input_fd);
    close(next_pipe[1]);
    sieve(next_pipe[0]);
  }

  close(next_pipe[0]);
  while(read(input_fd, &number, sizeof(number)) == sizeof(number))
  {
    if(number % prime != 0)
    {
      if(write(next_pipe[1], &number, sizeof(number)) != sizeof(number))
      {
        fprintf(2, "primes: write failed\n");
        close(input_fd);
        close(next_pipe[1]);
        wait(0);
        exit(1);
      }
    }
  }

  close(input_fd);
  close(next_pipe[1]);
  wait(0);
  exit(0);
}

int main(int argc, char *argv[])
{
  int first_pipe[2];
  int pid;
  int number;

  if(pipe(first_pipe) < 0)
  {
    fprintf(2, "primes: pipe failed\n");
    exit(1);
  }

  pid = fork();
  if(pid < 0)
  {
    fprintf(2, "primes: fork failed\n");
    close(first_pipe[0]);
    close(first_pipe[1]);
    exit(1);
  }

  if(pid == 0)
  {
    close(first_pipe[1]);
    sieve(first_pipe[0]);
  }

  close(first_pipe[0]);
  for(number = 2; number <= 35; number++)
  {
    if(write(first_pipe[1], &number, sizeof(number)) != sizeof(number)){
      fprintf(2, "primes: write failed\n");
      close(first_pipe[1]);
      wait(0);
      exit(1);
    }
  }

  close(first_pipe[1]);
  wait(0);
  exit(0);
}