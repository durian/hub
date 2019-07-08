#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <poll.h>
#include <assert.h>
#include <unistd.h>
#include <fcntl.h>

#include <nanomsg/nn.h>
#include <nanomsg/bus.h>
#include <nanomsg/inproc.h>
#include <jansson.h>

void print_selection(json_t *root, const char *select) {
    while (1) {
        char *linebreak = strchr(select, '.');
        if (linebreak != 0) *linebreak = 0;
        root = json_object_get(root, select);
        select = linebreak;
        if (linebreak == 0) break;
    }
        
    if (root == NULL) {
    } else if (root->type == JSON_STRING) {
        printf("%s\n", json_string_value(root));
    } else {
        char *json_value = json_dumps(root, JSON_ENCODE_ANY);
        printf("%.*s\n", (int) strlen(json_value), json_value);
    }    
}

int streq(char *check, const char *stat) {
    int size1 = strlen(check);
    int size2 = strlen(stat);
    return size1 == size2 && 0 == strncmp(check, stat, size2);
}

int main(int argc, char **argv) {
    int msgid = rand();
    const char *select = NULL;

    json_t *obj = json_object();
    json_object_set(obj, "command", json_string(argv[1]));
    json_object_set(obj, "id", json_integer(msgid));
    for (int i=2; i<argc;) {
        if (streq(argv[i], "--param-float")) {
            double result;
            sscanf(argv[i + 2], "%lf", &result);
            json_object_set(obj, argv[i + 1], json_real(result));
            i += 3;
        }
        else if (streq(argv[i], "--param-int")) {
            int result;
            sscanf(argv[i + 2], "%d", &result);
            json_object_set(obj, argv[i + 1], json_integer(result));
            i += 3;
        }
        else if (streq(argv[i], "--param-bool")) {
            int is_true = 0 == strncmp(argv[i + 2], "true", strlen(argv[i + 2]));
            json_object_set(obj, argv[i + 1], json_boolean(is_true));
            i += 3;
        }
        else if (streq(argv[i], "--param")) {
            json_object_set(obj, argv[i + 1], json_string(argv[i + 2]));
            i += 3;
        }
        else if (streq(argv[i], "--select")) {
            select = argv[i + 1];
            i += 2;
        }
        else {
            fprintf(stderr, "invalid argument: %s\n", argv[i]);
            exit(2);
        }
    }

    char *json_value = json_dumps(obj, 0);
    json_decref(obj);

    int nano_socket = nn_socket(AF_SP, NN_BUS);
    if (nano_socket < 0) {
        fprintf(stderr, "%s", "could not create nanomsg socket");
        return 1;
    }
    assert(nn_connect(nano_socket, "ipc:///tmp/cloud.ipc"));
    sleep(1);

    char buf[8192];
    sprintf(buf, "CALL %s\n", json_value);
    free(json_value);
    nn_send(nano_socket, buf, strlen(buf), 0);

    while(1) {
        char recv_buf[10 * 8192];
        int recv_len = nn_recv(nano_socket, recv_buf, sizeof(recv_buf), 0);
        if (recv_len <= 0) break;
        if (0 != strncmp("OK{", recv_buf, 3)) continue;
        
        json_error_t error;
        json_t *root = json_loads(recv_buf + 2, 0, &error);
        if (!root) continue;

        json_t *idobj = json_object_get(root, "id");
        int id = json_number_value(idobj);
        if (id != msgid) continue;

        if (select == NULL) printf("%.*s\n", recv_len - 2, recv_buf + 2);
        else print_selection(root, select);
        json_decref(root);
        break;
    }
    return 0;
}
