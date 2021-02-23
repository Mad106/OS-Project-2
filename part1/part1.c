#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

const char DIR[] = "test_dir";
const char MESSAGE[18] = "Hello from Part 1";

/*
 This program:
 (1) Creates new directory "test_dir"
 (2) Creates new or open existing file "test_dir/part1.txt"
 (3) Write message "Hello from Part 1" to file
 (4) Closes file
*/

// Syscall 1: mkdir()
// https://man7.org/linux/man-pages/man2/mkdirat.2.html

// Syscall 2: open()
// https://man7.org/linux/man-pages/man2/open.2.html

// Syscall 3: write()
// https://www.man7.org/linux/man-pages/man2/write.2.html

// Syscall 4: close()
// https://man7.org/linux/man-pages/man2/close.2.html

int main()
{
    mkdir("test_dir", 0777); // Syscall 1: mkdir()

    int fd = open("test_dir/part1.txt", O_WRONLY | O_TRUNC | O_CREAT, 0777); // Syscall 2: open()

    write(fd, MESSAGE, 18 - 1); // Sysccall 3: write()

    close(fd); // Sysccall 4: close()

    return 0;
}
