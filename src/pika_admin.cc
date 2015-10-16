#include <algorithm>
#include <glog/logging.h>
#include <map>
#include <sys/utsname.h>
#include <sys/types.h>
#include <unistd.h>

#include "pika_command.h"
#include "pika_server.h"
#include "pika_conf.h"
#include "pika_define.h"
#include "util.h"

extern PikaServer *g_pikaServer;
extern std::map<std::string, Cmd *> g_pikaCmd;
extern PikaConf* g_pikaConf;

void AuthCmd::Do(std::list<std::string> &argv, std::string &ret) {
    if ((arity > 0 && (int)argv.size() != arity) || (arity < 0 && (int)argv.size() < -arity)) {
        ret = "-ERR wrong number of arguments for ";
        ret.append(argv.front());
        ret.append(" command\r\n");
        return;
    }
    argv.pop_front();
    std::string password = argv.front();
    argv.pop_front();
    if (password == std::string(g_pikaConf->requirepass()) || std::string(g_pikaConf->requirepass()) == "") {
        ret = "+OK\r\n";
    } else {
        ret = "-ERR invalid password\r\n";
    }
}

void PingCmd::Do(std::list<std::string> &argv, std::string &ret) {
    if ((arity > 0 && (int)argv.size() != arity) || (arity < 0 && (int)argv.size() < -arity)) {
        ret = "-ERR wrong number of arguments for ";
        ret.append(argv.front());
        ret.append(" command\r\n");
        return;
    }
    argv.pop_front();
    ret = "+PONG\r\n";
}

void ClientCmd::Do(std::list<std::string> &argv, std::string &ret) {
    if ((argv.size() != 2 && argv.size() != 3) || (arity > 0 && (int)argv.size() != arity) || (arity < 0 && (int)argv.size() < -arity)) {
        ret = "-ERR wrong number of arguments for ";
        ret.append(argv.front());
        ret.append(" command\r\n");
        return;
    }
    argv.pop_front();
    std::string opt = argv.front();
    argv.pop_front();
    transform(opt.begin(), opt.end(), opt.begin(), ::tolower);
    if (opt == "list") {
        g_pikaServer->ClientList(ret);
    } else if (opt == "kill") {
        std::string ip_port = argv.front();
        argv.pop_front();
        int res = g_pikaServer->ClientKill(ip_port);
        if (res == 1) {
            ret = "+OK\r\n";
        } else {
            ret = "-ERR No such client\r\n";
        }
        
    } else {
        ret = "-ERR Syntax error, try CLIENT (LIST | KILL ip:port)\r\n"; 
    }

}

void BemasterCmd::Do(std::list<std::string> &argv, std::string &ret) {
    if ((arity > 0 && (int)argv.size() != arity) || (arity < 0 && (int)argv.size() < -arity)) {
        ret = "-ERR wrong number of arguments for ";
        ret.append(argv.front());
        ret.append(" command\r\n");
        return;
    }
    argv.pop_front();
    std::string str_fd = argv.front();
    argv.pop_front();
    int64_t fd;
    if (!string2l(str_fd.data(), str_fd.size(), &fd)) {
        ret = "-ERR value is not an integer or out of range\r\n";
        return;
    }

    int res = g_pikaServer->ClientRole(fd, CLIENT_MASTER);
    if (res == 1) {
        ret = "+OK\r\n";
    } else {
        ret = "-ERR No such client\r\n";
    }
}

void SlaveofCmd::Do(std::list<std::string> &argv, std::string &ret) {
    if ((arity > 0 && (int)argv.size() != arity) || (arity < 0 && (int)argv.size() < -arity)) {
        ret = "-ERR wrong number of arguments for ";
        ret.append(argv.front());
        ret.append(" command\r\n");
        return;
    }
    argv.pop_front();
    std::string ip = argv.front();
    argv.pop_front();
    std::string str_port = argv.front();
    argv.pop_front();
    int64_t port = 0;
    if (!string2l(str_port.data(), str_port.size(), &port)) {
        ret = "-ERR value is not an integer or out of range\r\n";
        return;
    }
    
    std::string hostip = g_pikaServer->GetServerIp();
    if ((ip == hostip || ip == "127.0.0.1") && port == g_pikaServer->GetServerPort()) {
        ret = "-ERR you fucked up\r\n";
        return;
    }

    {
    MutexLock l(g_pikaServer->Mutex());
    if (g_pikaServer->masterhost() == ip && g_pikaServer->masterport() == port) {
        ret = "+OK Already connected to specified master\r\n";
        return;
    } else if (g_pikaServer->repl_state() != PIKA_SINGLE) { 
        ret = "-ERR State is not in PIKA_SINGLE\r\n";
        return;
    }
    struct sockaddr_in s_addr;
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    memset(&s_addr, 0, sizeof(s_addr));
    if (sock == -1) {
        ret = "-ERR socket fail\r\n";
        return;
    }
    s_addr.sin_family = AF_INET;
    s_addr.sin_addr.s_addr = inet_addr(ip.c_str());
    s_addr.sin_port = htons(port);
    if (-1 == connect(sock, (struct sockaddr*)(&s_addr), sizeof(s_addr))) {
        ret = "-ERR connect fail\r\n";
        return;
    }
    std::string message = "*3\r\n$8\r\npikasync\r\n";
    char buf[32];
    snprintf(buf, sizeof(buf), "$%lu\r\n", (g_pikaServer->GetServerIp()).length());
    message.append(buf);
    message.append(g_pikaServer->GetServerIp());
    message.append("\r\n");
    std::string str_serverport;
    char buf_len[32];
    int len = ll2string(buf, sizeof(buf), g_pikaServer->GetServerPort());
    snprintf(buf_len, sizeof(buf_len), "$%d\r\n", len);
    message.append(buf_len);
    message.append(buf);
    message.append("\r\n");
    sds wbuf = sdsempty();
    wbuf = sdscatlen(wbuf, message.data(), message.size());
    ssize_t nwritten = 0;
    while (sdslen(wbuf) > 0) {
        nwritten = write(sock, wbuf, sdslen(wbuf));
        if (nwritten == -1) {
            if (errno == EAGAIN) {
                nwritten = 0;
            } else {
                /*
                 * Here we clear this connection
                 */
                ret = "-ERR error in sending to master\r\n";
                return;
            }
        }
        if (nwritten < 0) {
            break;
        }
        sdsrange(wbuf, nwritten, -1);
    }
    if (sdslen(wbuf) == 0) {
        ret = "+OK\r\n";
        g_pikaServer->set_repl_state(PIKA_SLAVE);
        g_pikaServer->set_masterhost(ip);
        g_pikaServer->set_masterport(port);
    } else {
        ret = "-ERR error in sending to master\r\n";
    }

    }
}

void PikasyncCmd::Do(std::list<std::string> &argv, std::string &ret) {
    if ((arity > 0 && (int)argv.size() != arity) || (arity < 0 && (int)argv.size() < -arity)) {
        ret = "-ERR wrong number of arguments for ";
        ret.append(argv.front());
        ret.append(" command\r\n");
        return;
    }
    argv.pop_front();
    std::string ip = argv.front();
    argv.pop_front();
    std::string str_port = argv.front();
    argv.pop_front();
    int64_t port = 0;
    if (!string2l(str_port.data(), str_port.size(), &port)) {
        ret = "-ERR value is not an integer or out of range\r\n";
        return;
    }
//    LOG(INFO) << ip << " : " << str_port;
    std::string slave_key = ip + ":" + str_port;
    int res = g_pikaServer->TrySync(ip, str_port);
    if (res == PIKA_REP_STRATEGY_ALREADY) {
        ret = "-ERR Already\r\n";
    } else if (res == PIKA_REP_STRATEGY_PSYNC) {
        ret = "+OK PSYNC\r\n";
    } else {
        ret = "-ERR Error\r\n";
    }
}

void PutInt32(std::string *dst, int32_t v) {
    std::string vstr = std::to_string(v);
    dst->append(vstr);
}

void EncodeString(std::string *dst, const std::string &value) {
    dst->append("$");
    PutInt32(dst, value.size());
    dst->append("\r\n");
    dst->append(value.data(), value.size());
    dst->append("\r\n");
}

void EncodeInt32(std::string *dst, const int32_t v) {
    std::string vstr = std::to_string(v);
    dst->append("$");
    PutInt32(dst, vstr.length());
    dst->append("\r\n");
    dst->append(vstr);
    dst->append("\r\n");
}

void ConfigCmd::Do(std::list<std::string> &argv, std::string &ret) {
    if (argv.size() != 3 && argv.size() != 4) {
        ret = "-ERR wrong number of arguments for 'config' command\r\n";
        return;
    }
    argv.pop_front();
    std::string opt = argv.front();
    argv.pop_front();
    transform(opt.begin(), opt.end(), opt.begin(), ::tolower);

    std::string conf_item = argv.front();
    argv.pop_front();
    transform(conf_item.begin(), conf_item.end(), conf_item.begin(), ::tolower);
  
    if (argv.size() == 0 && opt == "get") {
        if (conf_item == "port") {
            ret = "*2\r\n";
            EncodeString(&ret, "port");
            EncodeInt32(&ret, g_pikaConf->port());
        } else if (conf_item == "thread_num") {
            ret = "*2\r\n";
            EncodeString(&ret, "thread_num");
            EncodeInt32(&ret, g_pikaConf->thread_num());
        } else if (conf_item == "log_path") {
            ret = "*2\r\n";
            EncodeString(&ret, "log_path");
            EncodeString(&ret, g_pikaConf->log_path());
        } else if (conf_item == "log_level") {
            ret = "*2\r\n";
            EncodeString(&ret, "log_level");
            EncodeInt32(&ret, g_pikaConf->log_level());
        } else if (conf_item == "db_path") {
            ret = "*2\r\n";
            EncodeString(&ret, "db_path");
            EncodeString(&ret, g_pikaConf->db_path());
        } else if (conf_item == "write_buffer_size") {
            ret = "*2\r\n";
            EncodeString(&ret, "write_buffer_size");
            EncodeInt32(&ret, g_pikaConf->write_buffer_size());
        } else if (conf_item == "timeout") {
            ret = "*2\r\n";
            EncodeString(&ret, "timeout");
            EncodeInt32(&ret, g_pikaConf->timeout());
        } else if (conf_item == "requirepass") {
            ret = "*2\r\n";
            EncodeString(&ret, "requirepass");
            EncodeString(&ret, g_pikaConf->requirepass());
        } else {
            ret = "-ERR No such configure item\r\n";
        }
    } else if (argv.size() == 1 && opt == "set") {
        std::string value = argv.front();
        long int ival;
        if (conf_item == "timeout") {
            if (!string2l(value.data(), value.size(), &ival)) {
                ret = "-ERR Invalid argument " + value + " for CONFIG SET 'timeout'\r\n";
                return;
            }
            g_pikaConf->SetTimeout(ival);
            ret = "+OK\r\n";
        } else if (conf_item == "requirepass") {
            g_pikaConf->SetRequirePass(value);
            ret = "+OK\r\n";
        } else {
            ret = "-ERR No such configure item\r\n";
        }
    } else {
        ret = "-ERR wrong number of arguments for CONFIG ";
        ret.append(opt);
        ret.append(" command\r\n");
        return;
    }
}

void InfoCmd::Do(std::list<std::string> &argv, std::string &ret) {
    if (argv.size() != 1 && argv.size() != 2) {
        ret = "-ERR wrong number of arguments for 'info' command\r\n";
        return;
    }
    argv.pop_front();
    bool allsections = true;
    std::string section = "";
    int sections = 0;

    if (!argv.empty()) {
        allsections = false;
        section = argv.front();
        transform(section.begin(), section.end(), section.begin(), ::tolower);
    }

    std::string info;
    // Server
    if (allsections || section == "server") {
        //TODO mode : standalone cluster sentinel

        if (sections++) info.append("\r\n");

        static bool call_uname = true;
        static struct utsname name;
        if (call_uname) {
            uname(&name);
            call_uname = false;
        }

        char buf[512];
        snprintf (buf, sizeof(buf),
                  "# Server\r\n"
                  "pika_version:%s\r\n"
                  "os:%s %s %s\r\n"
                  "process_id:%ld\r\n"
                  "tcp_port:%d\r\n"
                  "thread_num:%d\r\n"
                  "config_file:%s\r\n",
                  PIKA_VERSION,
                  name.sysname, name.release, name.machine,
                  (long) getpid(),
                  g_pikaConf->port(),
                  g_pikaConf->thread_num(),
                  g_pikaConf->conf_path());
        info.append(buf);
    }

    // Clients
    if (allsections || section == "clients") {
        if (sections++) info.append("\r\n");

        std::string clients;
        char buf[128];
        snprintf (buf, sizeof(buf),
                  "# Clients\r\n"
                  "connected_clients:%d\r\n",
                  g_pikaServer->ClientList(clients));
        info.append(buf);
    }

    // Stats
  //  if (allsections || section == "stats") {
  //      if (sections++) info.append("\r\n");
  //  }

    ret.clear();
    char buf[32];
    snprintf (buf, sizeof(buf), "$%u\r\n", info.length());
    ret.append(buf);
    ret.append(info);
    ret.append("\r\n");
}