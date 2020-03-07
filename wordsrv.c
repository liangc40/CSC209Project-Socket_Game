#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <time.h>

#include "socket.h"
#include "gameplay.h"


#ifndef PORT
    #define PORT 50121
#endif
#define MAX_QUEUE 5


void add_player(struct client **top, int fd, struct in_addr addr);
void remove_player(struct client **top, int fd);
void move_player(struct client **new_player, struct client **active_player, int fd);
/* Move the has_next_turn pointer to the next active client */
void advance_turn(struct game_state *game);
/* These are some of the function prototypes that we used in our solution 
 * You are not required to write functions that match these prototypes, but
 * you may find the helpful when thinking about operations in your program.
 */
/* Send the message in outbuf to all clients */
void broadcast(struct game_state *game, char *outbuf);
void broadcast_two_messages(struct game_state *game, char *first_msg, char *second_msg);
int read_newline(struct client *p, char *newline);
void disconnect_with_next_turn(struct game_state *game, struct client *p, char *first_msg);
void disconnect_without_next_turn(struct game_state *game, struct client *p,
    char *first_msg, char *second_msg);
void not_turn_to_guess(struct game_state *game, struct client *p, char *first_msg);
void handle_invalid_input(struct game_state *game, struct client *p,
    char letter, char *first_msg);
void guess_letter(struct game_state *game, struct client *p, char letter, char *first_msg, char *second_msg);
void operations_after_each_turn(struct game_state *game, struct client *p, char *dict_name, char *first_msg, char *second_msg);
void handle_valid_input(struct game_state *game, struct client *p, char letter, char *dict_name,
    char *first_msg, char *second_msg);
void handle_valid_input(struct game_state *game, struct client *p, char letter, char *dict_name,
    char *first_msg, char *second_msg);
void announce_turn(struct game_state *game, char *first_msg, char *second_msg);
void find_name(struct game_state *game, int exist, char *name);
void write_welcome_message(struct client **new_players, struct client *p);
void new_player_enter_game(struct client **new_players, struct game_state *game, struct client *p,
    char *first_msg, char *second_msg, char *newline);
void announce_winner(struct game_state *game, struct client *winner);




/* The set of socket descriptors for select to monitor.
 * This is a global variable because we need to remove socket descriptors
 * from allset when a write to a socket fails.
 */
fd_set allset;


/* Add a client to the head of the linked list
 */
void add_player(struct client **top, int fd, struct in_addr addr) {
    struct client *p = malloc(sizeof(struct client));

    if (!p) {
        perror("malloc");
        exit(1);
    }

    printf("Adding client %s\n", inet_ntoa(addr));

    p->fd = fd;
    p->ipaddr = addr;
    p->name[0] = '\0';
    p->in_ptr = p->inbuf;
    p->inbuf[0] = '\0';
    p->next = *top;
    *top = p;
}

/* Removes client from the linked list and closes its socket.
 * Also removes socket descriptor from allset 
 */
void remove_player(struct client **top, int fd) {
    struct client **p;

    for (p = top; *p && (*p)->fd != fd; p = &(*p)->next)
        ;
    // Now, p points to (1) top, or (2) a pointer to another client
    // This avoids a special case for removing the head of the list
    if (*p) {
        struct client *t = (*p)->next;
        printf("Removing client %d %s\n", fd, inet_ntoa((*p)->ipaddr));
        FD_CLR((*p)->fd, &allset);
        close((*p)->fd);
        free(*p);
        *p = t;
    } else {
        fprintf(stderr, "Trying to remove fd %d, but I don't know about it\n", fd);
    }
}

/* move a new_player to active player list */
void move_player(struct client **new_player, struct client **active_player, int fd){
    struct client **p;

    for (p = new_player; *p && (*p)->fd != fd; p = &(*p)->next);

    if (*p) {
        struct client *t = (*p)->next;
        // p link active_player
        (*p)->next = *active_player;
        *active_player = *p;
        *p = t;
    } else {
        fprintf(stderr, "Trying to remove fd %d, but I don't know about it\n", fd);
    }
}

/* Move the has_next_turn pointer to the next active client */
void advance_turn(struct game_state *game) {
    if (game->has_next_turn == NULL || game->has_next_turn->next == NULL) {
        game->has_next_turn = game->head;
    }
    else {
        game->has_next_turn = game->has_next_turn->next;
    }
}

/* Send the message in outbuf to all clients */
void broadcast(struct game_state *game, char *outbuf) {
    struct client *p;
    for(p = game->head; p != NULL; p = p->next) {
        if(write(p->fd, outbuf, strlen(outbuf)) == -1) {
            fprintf(stderr, "Write to client %s failed\n", inet_ntoa(p->ipaddr));
            if (game->has_next_turn == p) {
                advance_turn(game);
            }
            remove_player(&(game->head), p->fd);
        }
    }
}

/* Send one message to the a certain client, and send another message to all
 * clients expect the client receving the first message.
 */
void broadcast_two_messages(struct game_state *game, char *first_msg, char *second_msg) {
    struct client *p;
    for(p = game->head; p != NULL; p = p->next) {
        if (game->has_next_turn == p) {
            if(write(p->fd, first_msg, strlen(first_msg)) == -1) {
                fprintf(stderr, "Write to client %s failed\n", inet_ntoa(p->ipaddr));
                advance_turn(game);
                remove_player(&(game->head), p->fd);
            }
        } else {
            if(write(p->fd, second_msg, strlen(second_msg)) == -1) {
                fprintf(stderr, "Write to client %s failed\n", inet_ntoa(p->ipaddr));
                remove_player(&(game->head), p->fd);
            }
        }
    }
}



/* Read from input and add the result to in_ptr. Once we've read the 
 * network newline then we successfully get one newline, and returns 0.
 * Otherwise, returns -1 if the client closes; and return -2 if the
 * newline is too long.
 */
int read_newline(struct client *p, char *newline) {
    int readcnt;
    int postion, i;
    int is_too_long = 0;
    while (1) {
        // if the read functions fails, indicating that the client closes.
        readcnt = read(p->fd, p->in_ptr, MAX_BUF-strlen(p->inbuf)-1);
        if (readcnt <= 0) {
            printf("[%d] Read 0 bytes\n", p->fd);
            return -1;
        }
        printf("[%d] Read %d bytes\n", p->fd, readcnt);
        p->in_ptr = p->in_ptr+readcnt;
        *p->in_ptr = '\0';

        
        // Now find the network newline \r\n.
        postion = -1;
        for (postion = 0 ; postion < strlen(p->inbuf)-1; postion++) {
            if (p->inbuf[postion] == '\r' && p->inbuf[postion+1] == '\n') {
                //find end postion
                break;
            }
        }
        // if the network newline does not exist or the position of \r is
        // at the end of inbuf
        if (postion == -1 || postion == strlen(p->inbuf)-1){
            // not find, continue read
            if (strlen(p->inbuf) == MAX_BUF -1) {
                // inbuf full
                is_too_long = 1;
                // and clear inbuf
                p->in_ptr = p->inbuf;
            }
            continue;
        }

        // assign the new line
        for (i=0; i < postion; i ++) {
            newline[i] = p->inbuf[i];
        }
        newline[i] = '\0';

        // move remaining characters
        postion += 2;
        i = 0;
        while (postion < strlen(p->inbuf)) {
            p->inbuf[i] = p->inbuf[postion];
            postion ++;
            i ++;
        }
        p->inbuf[i] = '\0';
        p->in_ptr = p->inbuf + strlen(p->inbuf);
        
        printf("[%d] Found newline %s\n", p->fd, newline);
        break;
    }

    // if the new line is too long, returns -2
    if (is_too_long)
        return -2;

    return 0;
}

/* Close the socket and remove player when someone with the next turn disconnects. */
void disconnect_with_next_turn(struct game_state *game, struct client *p, char *first_msg) {
    printf("Disconnect from %s\n",inet_ntoa(p->ipaddr));
    // trun is change
    advance_turn(game);
    if (game->has_next_turn == p) {
        game->has_next_turn = NULL;
    }
    sprintf(first_msg,"Goodbye %s\r\n", p->name);
    remove_player(&(game->head), p->fd);
    broadcast(game, first_msg);
}

/* Close socket and remove player when someone without the next turn disconnects. */
void disconnect_without_next_turn(struct game_state *game, struct client *p,
    char *first_msg, char *second_msg) {
    printf("Disconnect from %s\n",inet_ntoa(p->ipaddr));
    sprintf(first_msg,"Goodbye %s\r\n", p->name);
    remove_player(&(game->head), p->fd);
    broadcast(game, first_msg);
    // ask turn player
    sprintf(first_msg, "Your guess?\r\n");
    sprintf(second_msg, "It's %s's turn.\r\n", (game->has_next_turn)->name);
    broadcast_two_messages(game, first_msg, second_msg);
}

/* Show someone who is not the next turn that the next turn is not him/her. */
void not_turn_to_guess(struct game_state *game, struct client *p, char *first_msg) {
    sprintf(first_msg, "It is not your turn to guess.\r\n");
    printf("Player %s tried to guess out of turn\n", p->name);
    if(write(p->fd, first_msg, strlen(first_msg)) == -1) {
        fprintf(stderr, "Write to client %s failed\n", inet_ntoa(p->ipaddr));
        remove_player(&(game->head), p->fd);
    }
}

/* Handle the case where the input letter is invalid.  */
void handle_invalid_input(struct game_state *game, struct client *p,
    char letter, char *first_msg) {
    // guess it
    sprintf(first_msg, "%s guesses: %c\r\n", p->name, letter);
    int i;
    for(i=0;i<strlen(game->guess); ++i) {
        if (letter == (game->word)[i]) {
        (game->guess[i]) = letter;
        }
    }
    broadcast(game, first_msg);
}

/* If the letter to be guessed, then guess this letter. There are two cases to analyze: The letter is indeed
 * in the word and has not been guessed yet, and the case where letter is not in the word or the letter to
 * guess has already been guessed.
 */
void guess_letter(struct game_state *game, struct client *p, char letter, char *first_msg, char *second_msg) {
    //if the letter has not been guessed yet and this letter is in the word
    if ((game->letters_guessed)[letter-'a'] == 0
        && strchr(game->word, letter) != NULL) {
        //then guess it
        sprintf(first_msg, "%s guesses: %c\r\n", p->name, letter);
        int i;
        //make the corresponding position in game->guess to the letter
        for(i=0;i<strlen(game->guess); ++i) {
            if (letter == (game->word)[i]) {
                (game->guess)[i] = letter;
            }
        }
        broadcast(game, first_msg);
    } else {
        // Otherwise, the letter is not in the word
        // error
        sprintf(first_msg, "%c is not in the word\r\n%s guesses: %c\r\n",
        letter, p->name, letter);
        printf("Letter %c is not in the word\n", letter);
        // notify all

        sprintf(second_msg, "%s guesses: %c\r\n", p->name, letter);
        broadcast_two_messages(game, first_msg, second_msg);
        // change turn
        advance_turn(game);
        game->guesses_left -= 1;
    }
    // mark this letter as guessed
    (game->letters_guessed)[letter-'a'] = 1;

}

/* Check whether after each turn players are running out of guesses or have correctly guessed the word;
 * if we've correctly guessed the word, then print the winning message; if players are running out of
 * guesses, print messages indicating there are no more guesses; and for both cases we should restart
 * the game; otherwise, print just print the state message of this turn and broadcast it.
 */
void operations_after_each_turn(struct game_state *game, struct client *p, char *dict_name, char *first_msg, char *second_msg) {
    // if we are running out of guesses or correctly guess the word, the game would terminate
    if (game->guesses_left == 0 || strchr(game->guess, '-') == NULL) {
        // the case when running out of guesses
        if (game->guesses_left == 0) {
            //sprintf(msg, "The word was %s.\r\nNo guesses left. Game over.\r\n", game.word);
            sprintf(first_msg, "No more guesses.  The word was %s\r\n", game->word);
            printf("Evaluating for game_over\n");
            broadcast(game, first_msg);
        } else {
            // the case when successfully guess the word
            sprintf(first_msg, "The word was %s.\r\nGame over! You win!\r\n", game->word);
            sprintf(second_msg, "The word was %s.\r\nGame over! %s win!\r\n", game->word, p->name);
            printf("Game over. %s won!\n",p->name);
            broadcast_two_messages(game, first_msg, second_msg);
        }

        // init the game
        printf("New game\n");
        init_game(game, dict_name);
        sprintf(first_msg, "\r\n\r\nLet's start a new game\r\n");
        // broadcast(game, first_msg);
        status_message(first_msg, game);
        broadcast(game, first_msg);
    } else {
        // print game state
        status_message(first_msg, game);
        broadcast(game, first_msg);
    }
}

/* Handle the case where the input letter is valid. We need to guess this letter first; then
 * do some operations after this turn of guess.
 */
void handle_valid_input(struct game_state *game, struct client *p, char letter, char *dict_name,
    char *first_msg, char *second_msg) {
    guess_letter(game, p, letter, first_msg, second_msg);
    operations_after_each_turn(game, p, dict_name, first_msg, second_msg);
}

/* Do the announcing work for players after each turn of the game. */
void announce_turn(struct game_state *game, char *first_msg, char *second_msg) {
    sprintf(first_msg, "Your guess?\r\n");
    sprintf(second_msg, "It's %s's turn.\r\n", (game->has_next_turn)->name);
    printf("It's %s's turn.\n", (game->has_next_turn)->name);
    broadcast_two_messages(game, first_msg, second_msg);
}

/* Check whether the name newly entered has already appeared among players. */
void find_name(struct game_state *game, int exist, char *name) {
    struct client *temp;
    for(temp = game->head; temp != NULL; temp = temp->next) {
        if (strcmp(temp->name, name) == 0) {
            exist = 1;
            break;
        }
    }
}

/* Write welcome message to new players. */
void write_welcome_message(struct client **new_players, struct client *p) {
    char *greeting = WELCOME_MSG;
    if(write(p->fd, greeting, strlen(greeting)) == -1) {
        fprintf(stderr, "Write to client %s failed\n", inet_ntoa(p->ipaddr));
        // remove client p from new players list
        remove_player(new_players, p->fd);
    }
}

/* Handle the case where the input name is valid. We first make this new player to active player
 * list, then notify other players, then print the game state and announce turn.
 */
void new_player_enter_game(struct client **new_players, struct game_state *game, struct client *p,
    char *first_msg, char *second_msg, char *newline) {
    // set name
    strcpy(p->name, newline);
    // new player to active player
    move_player(new_players, &(game->head), p->fd);
    // init turn
    if ((game->has_next_turn) == NULL) {
        game->has_next_turn = p;
    }

    // notify all player, who enters the game
    sprintf(first_msg, "%s has just joined.\r\n", newline);
    printf("%s has just joined.\n", newline);
    broadcast(game, first_msg);
    // print  game state
    status_message(first_msg, game);
    if(write(p->fd, first_msg, strlen(first_msg)) == -1) {
        fprintf(stderr, "Write to client %s failed\n", inet_ntoa(p->ipaddr));
        if ((game->has_next_turn) == p) {
            advance_turn(game);
        }
        remove_player(&(game->head), p->fd);
    }

    announce_turn(game, first_msg, second_msg);
}


int main(int argc, char **argv) {
    int clientfd, maxfd, nready;
    struct client *p;
    struct sockaddr_in q;
    fd_set rset;
    
    if(argc != 2){
        fprintf(stderr,"Usage: %s <dictionary filename>\n", argv[0]);
        exit(1);
    }
    
    // Create and initialize the game state
    struct game_state game;

    srandom((unsigned int)time(NULL));
    // Set up the file pointer outside of init_game because we want to 
    // just rewind the file when we need to pick a new word
    game.dict.fp = NULL;
    game.dict.size = get_file_length(argv[1]);

    init_game(&game, argv[1]);
    
    // head and has_next_turn also don't change when a subsequent game is
    // started so we initialize them here.
    game.head = NULL;
    game.has_next_turn = NULL;
    
    /* A list of client who have not yet entered their name.  This list is
     * kept separate from the list of active players in the game, because
     * until the new playrs have entered a name, they should not have a turn
     * or receive broadcast messages.  In other words, they can't play until
     * they have a name.
     */
    struct client *new_players = NULL;
    
    struct sockaddr_in *server = init_server_addr(PORT);
    int listenfd = set_up_server_socket(server, MAX_QUEUE);
    
    // initialize allset and add listenfd to the
    // set of file descriptors passed into select
    FD_ZERO(&allset);
    FD_SET(listenfd, &allset);
    // maxfd identifies how far into the set to search
    maxfd = listenfd;

    while (1) {
        // make a copy of the set before we pass it into select
        rset = allset;
        nready = select(maxfd + 1, &rset, NULL, NULL, NULL);
        if (nready == -1) {
            perror("select");
            continue;
        }

        if (FD_ISSET(listenfd, &rset)){
            printf("A new client is connecting\n");
            clientfd = accept_connection(listenfd);

            FD_SET(clientfd, &allset);
            if (clientfd > maxfd) {
                maxfd = clientfd;
            }
            printf("Connection from %s\n", inet_ntoa(q.sin_addr));
            add_player(&new_players, clientfd, q.sin_addr);
            char *greeting = WELCOME_MSG;
            if(write(clientfd, greeting, strlen(greeting)) == -1) {
                fprintf(stderr, "Write to client %s failed\n", inet_ntoa(q.sin_addr));
                remove_player(&(game.head), p->fd);
            };
        }
        
        /* Check which other socket descriptors have something ready to read.
         * The reason we iterate over the rset descriptors at the top level and
         * search through the two lists of clients each time is that it is
         * possible that a client will be removed in the middle of one of the
         * operations. This is also why we call break after handling the input.
         * If a client has been removed the loop variables may not longer be 
         * valid.
         */
        int cur_fd;
        for(cur_fd = 0; cur_fd <= maxfd; cur_fd++) {
            if(FD_ISSET(cur_fd, &rset)) {
                // Check if this socket descriptor is an active player
                char newline[MAX_BUF];
                char first_msg[MAX_BUF];
                char second_msg[MAX_BUF];
                // struct client *temp;
                
                for(p = game.head; p != NULL; p = p->next) {
                    if(cur_fd == p->fd) {
                        //TODO - handle input from an active client
                        int result = read_newline(p, newline);
                        char letter = newline[0];
                        if (p == game.has_next_turn) {
                            // if cannot write to this player, meaning that the player disconnets
                            if (result == -1) {
                                disconnect_with_next_turn(&game, p, first_msg);    
                            } else {
                                // if the input is invalid
                                if (result == -2 || strlen(newline) != 1
                                    || letter < 'a' || letter > 'z') {
                                    handle_invalid_input(&game, p, letter, first_msg);
                                } else {
                                    // if the input letter is valid
                                    char *dict_name = argv[1];
                                    handle_valid_input(&game, p, letter, dict_name, first_msg, second_msg);
                                }
                            }
                            // do the announcing work for this turn
                            if (game.has_next_turn != NULL) {
                                announce_turn(&game, first_msg, second_msg);
                            }
                            
                        } else {
                            if (result == -1) {
                                disconnect_without_next_turn(&game, p, first_msg, second_msg);
                            } else {
                                not_turn_to_guess(&game, p, first_msg);
                            }
                        }
                        break;
                    }
                }
        
                // Check if any new players are entering their names
                for(p = new_players; p != NULL; p = p->next) {
                    if(cur_fd == p->fd) {
                        // TODO - handle input from an new client who has
                        // not entered an acceptable name.
                        int result = read_newline(p, newline);
                        if (result == -1) {
                            // close socket
                            printf("Disconnect from %s\n",inet_ntoa(p->ipaddr));
                            remove_player(&new_players, p->fd);
                        }
                        else {
                            int exist = 0;
                            find_name(&game, exist, newline);
                            // write welcome messsage to new players
                            if (result == -2 || exist == 1 || strlen(newline) == 0) {
                                write_welcome_message(&new_players, p);
                            } else {
                                new_player_enter_game(&new_players, &game, p, first_msg, second_msg, newline);
                            }
                        }
                        break;
                    } 
                }
            }
        }
    }
    return 0;
}
