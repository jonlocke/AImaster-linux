
#include <iostream>
#include <fstream>
#include <string>
#include <termios.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>

void configure_port(int fd, int baudrate) {
    struct termios tty;
    if (tcgetattr(fd, &tty) != 0) {
        std::cerr << "Error getting termios attributes: " << strerror(errno) << std::endl;
        exit(1);
    }

    cfsetospeed(&tty, baudrate);
    cfsetispeed(&tty, baudrate);

    tty.c_cflag &= ~PARENB; // No parity
    tty.c_cflag &= ~CSTOPB; // One stop bit
    tty.c_cflag &= ~CSIZE;
    tty.c_cflag |= CS8; // 8 bits
    tty.c_cflag &= ~CRTSCTS; // No flow control
    tty.c_cflag |= CREAD | CLOCAL; // Turn on READ & ignore ctrl lines

    tty.c_lflag &= ~ICANON;
    tty.c_lflag &= ~ECHO;
    tty.c_lflag &= ~ECHOE;
    tty.c_lflag &= ~ISIG;

    tty.c_iflag &= ~(IXON | IXOFF | IXANY); // No software flow control
    tty.c_iflag &= ~(ICRNL | INLCR); // Don't translate CR/LF

    tty.c_oflag &= ~OPOST; // Raw output

    tty.c_cc[VMIN] = 1; // Read blocks until at least 1 byte
    tty.c_cc[VTIME] = 0;

    if (tcsetattr(fd, TCSANOW, &tty) != 0) {
        std::cerr << "Error setting termios attributes: " << strerror(errno) << std::endl;
        exit(1);
    }
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <serial_port> <baudrate>" << std::endl;
        return 1;
    }

    std::string port = argv[1];
    int baudrate = std::stoi(argv[2]);

    int fd = open(port.c_str(), O_RDWR | O_NOCTTY | O_SYNC);
    if (fd < 0) {
        std::cerr << "Error opening " << port << ": " << strerror(errno) << std::endl;
        return 1;
    }

    configure_port(fd, baudrate);

    std::cout << "Serial passthrough test started on " << port << " at " << baudrate << " baud.\n";
    std::cout << "Type into this terminal to send data to the serial device.\n";

    fd_set readfds;
    char buf[256];

    while (true) {
        FD_ZERO(&readfds);
        FD_SET(fd, &readfds);
        FD_SET(STDIN_FILENO, &readfds);

        int maxfd = (fd > STDIN_FILENO ? fd : STDIN_FILENO) + 1;
        int activity = select(maxfd, &readfds, NULL, NULL, NULL);

        if (activity < 0 && errno != EINTR) {
            std::cerr << "select() error: " << strerror(errno) << std::endl;
            break;
        }

        if (FD_ISSET(fd, &readfds)) {
            int n = read(fd, buf, sizeof(buf));
            if (n > 0) {
                std::cout.write(buf, n);
                std::cout.flush();
            }
        }

        if (FD_ISSET(STDIN_FILENO, &readfds)) {
            int n = read(STDIN_FILENO, buf, sizeof(buf));
            if (n > 0) {
                write(fd, buf, n);
            }
        }
    }

    close(fd);
    return 0;
}
