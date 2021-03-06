#include "netbuffer.h"
#include "mailuser.h"
#include "server.h"
#include <sys/socket.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/utsname.h>
#include <ctype.h>
#include <strings.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h> 

#define MAX_LINE_LENGTH 1024

static void handle_client(int fd);

int main(int argc, char *argv[])
{

    if (argc != 2)
    {
        fprintf(stderr, "Invalid arguments. Expected: %s <port>\n", argv[0]);
        return 1;
    }

    run_server(argv[1], handle_client);

    return 0;
}

void send_invalid(int fd) {
    send_formatted(fd, "501 Invalid command.\r\n");
}

void send_valid(int fd) {
    send_formatted(fd, "250 OK\r\n");
}

void send_out_of_order(int fd) {
    send_formatted(fd, "503 Bad sequence of commands\r\n");
}

void free_users_lists(user_list_t* reverse_users_list, user_list_t* forward_users_list) {
    if (*reverse_users_list)
        destroy_user_list(*reverse_users_list);
    *reverse_users_list = NULL;
    if (*forward_users_list)
        destroy_user_list(*forward_users_list);
    *forward_users_list = NULL;
}

void handle_client(int fd)
{

    char recvbuf[MAX_LINE_LENGTH + 1];
    net_buffer_t nb = nb_create(fd, MAX_LINE_LENGTH);

    struct utsname my_uname;
    uname(&my_uname);
    char * domain = my_uname.nodename;

    // welcome message
    char *welcome_msg = "220 %s simple mail transfer protocol ready\r\n";
    send_formatted(fd, welcome_msg, domain);

    user_list_t reverse_users_list = NULL;
    user_list_t forward_users_list = NULL;
    
    int temp_fd;
    char file_name[] = "tempfile-XXXXXX";
    
    int data_mode = 0;
    int sent_helo = 0;
    int trans_mode = 0;

    while (1)
    {
        //  -1 is error, 0 is we are done
        int readlineVal = nb_read_line(nb, recvbuf);
        if (readlineVal == 0 || readlineVal == -1) {
            break;
        }

        // if command is too long send invalid
        if (recvbuf[readlineVal] == '\n') {
            send_invalid(fd);
            continue;
        }

        if (data_mode) {
            if (strcasecmp(recvbuf, ".\r\n") == 0) {
                // end of data command
                data_mode = 0;
                // send the mail
                close(temp_fd);
                save_user_mail(file_name, forward_users_list);
                
                // delete file
                unlink(file_name);
                trans_mode = 0;
                send_valid(fd);
            } else { // remove dot (first character)
                if (recvbuf[0] == '.') {
                    write(temp_fd, recvbuf + 1, readlineVal - 1);
                } else {
                    dlog("temp_fd: %i, %s, %i\n", temp_fd, recvbuf, readlineVal);
                    if (write(temp_fd, recvbuf, readlineVal) != readlineVal) {
                    dlog("Could not append line to file");
                    }
                }
            }
            continue;
        }

        char * parts[(MAX_LINE_LENGTH + 1) / 2];

        int splitCount = split(recvbuf, parts);
        dlog("%i\n", splitCount * 1);
        
        char * command = parts[0];

        if (command == NULL) {
            continue;
        }

        if (strcasecmp("NOOP", command) == 0) {
            // ignore extra params, still good
            send_valid(fd);
        } else if (strcasecmp("QUIT", command) == 0) {
            send_formatted(fd, "221 OK\r\n");
            break;
        } else if (strcasecmp("HELO", command) == 0 || strcasecmp("EHLO", command) == 0) {
            sent_helo = 1;
            trans_mode = 0;
            send_formatted(fd, "250 %s\r\n", domain);
        } else if (strcasecmp("VRFY", command) == 0) {
            if (splitCount != 2) {
                send_invalid(fd);
            } else {
                char * username_and_domain = parts[1];
                if (is_valid_user(username_and_domain, NULL)) {
                    send_formatted(fd, "250 %s\r\n", username_and_domain);
                } else {
                    send_formatted(fd, "550 user name does not exist\r\n");
                }
            }
        } else if (strcasecmp("MAIL", command) == 0) {
            // This command clears the reverse-path buffer, the forward-path buffer,
            // and the mail data buffer, and it inserts the reverse-path information
            // from its argument clause into the reverse-path buffer.

            // format: "MAIL FROM:<reverse-path> [SP <mail-parameters> ] <CRLF>"
            // helo must be sent first
            if (sent_helo == 0) {
                send_out_of_order(fd);
                continue;
            }
            
            // check for 1 param only and that we're not already in transaction mode
            if (splitCount != 2 || trans_mode == 1) {
                send_invalid(fd);
                continue;
            }

            free_users_lists(&reverse_users_list, &forward_users_list);

            // check if arg starts with FROM:<
            char* compare = malloc(7);
            *(compare + 6) = '\0';
            strncpy(compare,parts[1],6);
            int str_len = strlen(parts[1]);
            if (strcasecmp("FROM:<", compare) != 0 || ('>'!=parts[1][str_len-1])) {
                send_invalid(fd);
                continue;
            }

            // 6 is length of "FROM:<"
            const int FROM_SIZE = 6;
            char * str = malloc(str_len - FROM_SIZE - 1);
            strncpy(str, parts[1] + FROM_SIZE, str_len - FROM_SIZE - 1);
            add_user_to_list(&reverse_users_list, str);
            dlog("recieve: %s\n", str);
            free(str);

            trans_mode = 1;
            send_valid(fd);


        } else if (strcasecmp("RCPT", command) == 0) {
            // RCPT TO:<forward-path> [ SP <rcpt-parameters> ] <CRLF>
            if (sent_helo == 0) {
                send_out_of_order(fd);
                continue;
            }

            if (splitCount < 2) {
                send_invalid(fd);
                continue;
            }

            if (reverse_users_list == NULL) {
                send_out_of_order(fd);
            } else {
                // check if arg starts with TO:<
                char* compare = malloc(5);
                *(compare + 4) = '\0';
                strncpy(compare,parts[1],4);
                int str_len = strlen(parts[1]);
                if (strcasecmp("TO:<", compare) != 0 || ('>'!=parts[1][str_len-1])) {
                    send_invalid(fd);
                    continue;
                }

                // 6 is length of "TO:<"
                char * str = calloc(str_len - 4, 1);
                strncpy(str, parts[1] + 4, str_len - 4 - 1);
                dlog("forward: %s, str_len %i %i\n", str, str_len, str_len - 4 - 1);

                if (is_valid_user(str, NULL)) {
                    add_user_to_list(&forward_users_list, str);
                    send_valid(fd);
                } else {
                    send_formatted(fd, "550 user name does not exist\r\n");
                }
                free(str);
            }
        } else if (strcasecmp("DATA", command) == 0) {

            if (reverse_users_list == NULL || forward_users_list == NULL) {
                send_out_of_order(fd);
                continue;
            }

            // setup new file
            data_mode = 1;
            strcpy(file_name, "tempfile-XXXXXX");
            temp_fd = mkstemp(file_name);
            send_formatted(fd, "354 Enter mail, end with '.' on a line by itself.\r\n");
        } else if (strcasecmp("RSET", command) == 0) {
            
            if (splitCount != 1) {
                send_invalid(fd);
                continue;
            }

            free_users_lists(&reverse_users_list, &forward_users_list);
            
            trans_mode = 0;

            send_formatted(fd, "250 OK %s\r\n", domain);
        } else if (strcasecmp("EXPN", command) == 0 || strcasecmp("HELP", command) == 0) {
            send_formatted(fd, "502 %s Unsupported command.\r\n", domain);
        } else {
            send_formatted(fd, "500 %s Invalid command.\r\n", domain);
        }
    }

    nb_destroy(nb);
    return;
}
