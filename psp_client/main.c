#include <pspkernel.h>
#include <pspdisplay.h>
#include <psputility.h>
#include <psputility_modules.h>
#include <pspctrl.h>
#include <pspgu.h>
#include <pspgum.h>
#include <pspnet.h>
#include <pspnet_inet.h>
#include <pspnet_apctl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>

PSP_MODULE_INFO("SploomTerminal", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);
PSP_HEAP_SIZE_KB(-1024);

#define COLOR_TEAL   0xFFB8A300
#define COLOR_GREEN  0xFF66FF00
#define COLOR_WHITE  0xFFFFFFFF
#define COLOR_RED    0xFF0000FF
#define SCREEN_W     68

unsigned int __attribute__((aligned(16))) list[262144];
int current_draw = 0; 
int is_running = 1; 

volatile int net_request_active = 0;
volatile int net_status = 0; 
char net_target_endpoint[128];
char net_response_buffer[4096];

char om_ticker[8] = "      ";
int om_ticker_cursor = 0;       
int om_active_field = 0;        

int om_side = 0;                
int om_qty_type = 0;            
float om_qty = 1.0f;            
int om_type = 0;                
int om_tif = 0;                 

char* om_type_strs[] = {"MARKET", "LIMIT", "STOP", "STOP-LIMIT", "TRAILING-STOP"};
char* om_tif_strs[]  = {"DAY", "GTC", "FOK", "IOC", "OPG", "CLS"};

char om_status_msg[64] = "READY";

struct Vertex {
    unsigned int color; 
    float x, y, z;      
};                      

int NetworkThread(SceSize args, void *argp)
{
    static struct in_addr cached_ip;
    static int has_cached_ip = 0;

    while (is_running)
    {
        if (net_request_active == 1)
        {
            net_status = 1;

            int sock = sceNetInetSocket(AF_INET, SOCK_STREAM, 0);
            if (sock < 0) {
                strcpy(net_response_buffer, "SOCKET FAIL");
                net_status = -1; net_request_active = 0; continue;
            }

            if (!has_cached_ip) {
                unsigned char ip_bytes[4] = {127, 0, 0, 1}; /* REPLACE WITH YOUR BACKEND IP */
                memcpy(&cached_ip.s_addr, ip_bytes, 4);
                has_cached_ip = 1;
            }

            struct sockaddr_in serv_addr;
            memset(&serv_addr, 0, sizeof(serv_addr));
            serv_addr.sin_family = AF_INET;
            serv_addr.sin_port = htons(80);
            serv_addr.sin_addr.s_addr = cached_ip.s_addr;

            if (sceNetInetConnect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
                strcpy(net_response_buffer, "CONNECT FAILED");
                sceNetInetClose(sock);
                net_status = -1; net_request_active = 0; continue;
            }

            char request[256];
            snprintf(request, sizeof(request),
                "GET %s HTTP/1.0\r\n"
                "Host: your-backend-address.com\r\n"
                "Connection: close\r\n\r\n",
                net_target_endpoint);

            if (sceNetInetSend(sock, request, strlen(request), 0) < 0) {
                strcpy(net_response_buffer, "SEND FAILED");
                sceNetInetClose(sock);
                net_status = -1; net_request_active = 0; continue;
            }

            memset(net_response_buffer, 0, sizeof(net_response_buffer));
            
            int total = 0;

            while (is_running && total < (int)sizeof(net_response_buffer) - 1)
            {
                int bytes = sceNetInetRecv(sock, net_response_buffer + total, sizeof(net_response_buffer) - total - 1, 0);
                
                if (bytes > 0) {
                    total += bytes;
                } 
                else {
                    break; 
                } 
            }

            sceNetInetClose(sock);
            
            if (!is_running) break; 

            net_response_buffer[total] = '\0';
            
            if (total <= 0) { 
                strcpy(net_response_buffer, "RECV FAILED"); 
                net_status = -1; net_request_active = 0; continue; 
            }

            char *body = strstr(net_response_buffer, "\r\n\r\n");
            if (!body) { 
                strcpy(net_response_buffer, "BAD HEADER"); 
                net_status = -1; net_request_active = 0; continue; 
            }

            char temp_body[4096];
            char *src = body + 4;
            int dest_idx = 0;
            while(*src && dest_idx < 4095) {
                if(*src != '\r') { temp_body[dest_idx++] = *src; }
                src++;
            }
            temp_body[dest_idx] = '\0';
            strcpy(net_response_buffer, temp_body);

            net_status = 2;
            net_request_active = 0;
        }
        sceKernelDelayThread(10000);
    }
    return 0;
}

int exit_callback(int arg1, int arg2, void *common) { 
    is_running = 0; 
    sceKernelExitGame(); 
    return 0; 
}
int CallbackThread(SceSize args, void *argp) {
    int cbid = sceKernelCreateCallback("Exit Callback", exit_callback, NULL);
    sceKernelRegisterExitCallback(cbid); sceKernelSleepThreadCB(); return 0;
}
void SetupCallbacks() {
    int thid = sceKernelCreateThread("update_thread", CallbackThread, 0x11, 0xFA0, 0, 0);
    if(thid >= 0) sceKernelStartThread(thid, 0, 0);
    int net_thid = sceKernelCreateThread("network_thread", NetworkThread, 0x18, 0x10000, 0, 0);
    if(net_thid >= 0) sceKernelStartThread(net_thid, 0, 0);
}

void initGraphics() {
    sceGuInit(); sceGuStart(GU_DIRECT, list);
    sceGuDrawBuffer(GU_PSM_8888, (void*)0, 512);
    sceGuDispBuffer(480, 272, (void*)0x88000, 512); 
    sceGuDepthBuffer((void*)0x110000, 512);
    sceGuOffset(2048 - (480 / 2), 2048 - (272 / 2));
    sceGuViewport(2048, 2048, 480, 272);
    sceGuDepthRange(65535, 0);
    sceGuScissor(0, 0, 480, 272);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuFrontFace(GU_CW);
    sceGuClearColor(0xFF000000); sceGuClearDepth(0);
    sceGuFinish(); sceGuSync(0, 0); sceGuDisplay(GU_TRUE);
}

void drawFullLine(int y, u32 color, char symbol) {
    pspDebugScreenSetTextColor(color); pspDebugScreenSetXY(0, y);
    for(int i=0; i<SCREEN_W; i++) pspDebugScreenPrintf("%c", symbol);
}

void printCentered(int y, const char* text, u32 color) {
    int len = strlen(text); int x = (SCREEN_W - len) / 2; if(x < 0) x = 0;
    pspDebugScreenSetTextColor(color); pspDebugScreenSetXY(x, y);
    pspDebugScreenPrintf("%s", text);
}

void drawTopBar(int tab) {
    char menu[128];
    if(tab == 0) snprintf(menu, sizeof(menu), "[MAIN] MARKETS CHART NEWS ORDER");
    if(tab == 1) snprintf(menu, sizeof(menu), " MAIN [MARKETS] CHART NEWS ORDER");
    if(tab == 2) snprintf(menu, sizeof(menu), " MAIN MARKETS [CHART] NEWS ORDER");
    if(tab == 3) snprintf(menu, sizeof(menu), " MAIN MARKETS CHART [NEWS] ORDER");
    if(tab == 4) snprintf(menu, sizeof(menu), " MAIN MARKETS CHART NEWS [ORDER]");
    pspDebugScreenSetXY(0, 0); printCentered(0, menu, COLOR_WHITE);
    drawFullLine(1, COLOR_TEAL, '=');
}

int main() {
    SetupCallbacks(); initGraphics(); pspDebugScreenInit();
    
    sceGuStart(GU_DIRECT, list); sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT); sceGuFinish(); sceGuSync(0, 0);
    pspDebugScreenSetOffset(current_draw); pspDebugScreenSetXY(0, 0);
    printCentered(15, "SPLOOM TERMINAL", COLOR_WHITE);
    printCentered(17, ">>> PRESS [X] TO ESTABLISH LINK <<<", COLOR_TEAL);
    sceDisplayWaitVblankStart(); current_draw = (int)sceGuSwapBuffers();
    
    SceCtrlData pad, old_pad; old_pad.Buttons = 0;
    while(is_running) {
        sceCtrlReadBufferPositive(&pad, 1);
        if(pad.Buttons & PSP_CTRL_CROSS) break; 
        sceDisplayWaitVblankStart();
    }
    if(!is_running) goto shutdown;
    
    sceGuStart(GU_DIRECT, list); sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT); sceGuFinish(); sceGuSync(0, 0);
    pspDebugScreenSetOffset(current_draw); pspDebugScreenSetXY(0, 0);
    printCentered(15, "INITIATING HARDWARE...", COLOR_TEAL);
    sceDisplayWaitVblankStart(); current_draw = (int)sceGuSwapBuffers();
    
    sceUtilityLoadNetModule(PSP_NET_MODULE_COMMON); sceUtilityLoadNetModule(PSP_NET_MODULE_INET);
    sceNetInit(128*1024, 42, 4*1024, 42, 4*1024); sceNetInetInit(); sceNetApctlInit(0x2000, 42); 
    
    int state = 0, last_state = -1, retry = 0;
    while(is_running) {
        sceNetApctlGetState(&state);
        if(state != last_state) {
            last_state = state;
            sceGuStart(GU_DIRECT, list); sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT); sceGuFinish(); sceGuSync(0, 0);
            pspDebugScreenSetOffset(current_draw); pspDebugScreenSetXY(0, 0);
            
            if(state == 0) {
                if(retry > 0) { printCentered(15, "CONNECTION DROPPED. RETRYING...", COLOR_RED); sceDisplayWaitVblankStart(); current_draw = (int)sceGuSwapBuffers(); sceKernelDelayThread(3000000); }
                sceNetApctlConnect(1); retry++;
            }
            else if(state == 1) printCentered(15, "SCANNING FOR ROUTER...", COLOR_TEAL);
            else if(state == 2) printCentered(15, "AUTHENTICATING...", COLOR_TEAL);
            else if(state == 3) printCentered(15, "REQUESTING IP...", COLOR_TEAL);
            else if(state == 4) break;
            if(state != 0) { sceDisplayWaitVblankStart(); current_draw = (int)sceGuSwapBuffers(); }
        }
        sceKernelDelayThread(50000);
    }
    if(!is_running) goto shutdown;
    
    for(int i = 0; i < 60 && is_running; i++) {
        sceGuStart(GU_DIRECT, list); sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT); 
        pspDebugScreenSetOffset(current_draw); pspDebugScreenSetXY(0, 0);
        printCentered(15, "STABILIZING CONNECTION...", COLOR_TEAL);
        sceGuFinish(); sceGuSync(0, 0);
        sceDisplayWaitVblankStart(); current_draw = (int)sceGuSwapBuffers();
    }
    
    char api_data[4096];
    char ticker_list[20][16];
    int total_tickers = 0, current_ticker_idx = 0;
    
    int current_tab = 0; 
    int force_refresh = 0; 
    int active_fetch_tab = -1; 
    
    char ranges[5][4] = {"1D", "1W", "1M", "1Y", "ALL"};
    int current_range_idx = 1; 

    int internal_loading_frame = 0;
    int app_ready = 0; 
    
    char t0_acc[32] = "0", t0_buy[32] = "0";
    char t0_holdings[15][64]; 
    int t0_holdings_count = 0;
    char t0_orders[10][64]; 
    int t0_orders_count = 0;

    char t1_price[32]="-", t1_change[32]="-", t1_pct[32]="-", t1_high[32]="-", t1_low[32]="-", t1_vol[32]="-";
    char t1_pe[16]="-", t1_pb[16]="-", t1_eps[16]="-", t1_div[16]="-", t1_mcap[16]="-", t1_beta[16]="-";
    char t1_sec[32]="-", t1_ind[32]="-";

    int chart_points[100]; 
    int chart_sma[100]; 
    int chart_ubb[100]; 
    int chart_lbb[100]; 
    int chart_point_count = 0; 
    char max_price_str[64] = ""; 
    char min_price_str[64] = "";

    char t3_source[10][16];
    char t3_sentiment[10][8];
    char t3_headline[10][64];
    char t3_desc[10][512];
    int t3_count = 0;
    int news_cursor = 0;
    int news_reading_mode = 0;
    
    snprintf(net_target_endpoint, sizeof(net_target_endpoint), "/api/tickers");
    net_request_active = 1;
    
    while(is_running) {
        sceCtrlReadBufferPositive(&pad, 1);
        u32 pressed = pad.Buttons & ~old_pad.Buttons;
        
        if(app_ready == 0) {
            if(net_status == 2 || net_status == -1) {
                if(net_status == 2) {
                    snprintf(api_data, sizeof(api_data), "%.4095s", (char*)net_response_buffer);
                    char *save_tok;
                    char *tok = strtok_r(api_data, ",", &save_tok);
                    while(tok != NULL && total_tickers < 20) {
                        char clean_tok[16]; int j = 0;
                        for(int i=0; tok[i] != '\0' && i < 15; i++) {
                            if(tok[i] != '\n' && tok[i] != ' ' && tok[i] != '"' && tok[i] != '\'' && tok[i] != '[' && tok[i] != ']') { clean_tok[j++] = tok[i]; }
                        }
                        clean_tok[j] = '\0';
                        if(strlen(clean_tok) > 0) { snprintf(ticker_list[total_tickers], sizeof(ticker_list[0]), "%.15s", clean_tok); total_tickers++; }
                        tok = strtok_r(NULL, ",", &save_tok);
                    }
                    if(total_tickers == 0) { 
                        snprintf(ticker_list[0], sizeof(ticker_list[0]), "%.14s", api_data); 
                        total_tickers = 1; 
                    }
                    
                    memset(om_ticker, ' ', 6);
                    om_ticker[6] = '\0';
                    for(int i = 0; i < 6 && ticker_list[0][i] != '\0'; i++) {
                        om_ticker[i] = ticker_list[0][i];
                    }
                } else {
                    strcpy(ticker_list[0], "NET_FAILED"); total_tickers = 1;
                }
                app_ready = 1; net_status = 0; force_refresh = 1; 
            }
        } 
        else {
            if(net_status == 0 || net_status == 2 || net_status == -1) {
                if(pressed & PSP_CTRL_LTRIGGER) { current_tab--; if(current_tab < 0) current_tab = 4; force_refresh = 1; }
                if(pressed & PSP_CTRL_RTRIGGER) { current_tab++; if(current_tab > 4) current_tab = 0; force_refresh = 1; }
                
                if(current_tab != 4) {
                    if(pressed & PSP_CTRL_RIGHT) { current_ticker_idx++; if(current_ticker_idx >= total_tickers) current_ticker_idx = 0; force_refresh = 1; }
                    if(pressed & PSP_CTRL_LEFT) { current_ticker_idx--; if(current_ticker_idx < 0) current_ticker_idx = total_tickers - 1; force_refresh = 1; }
                    
                    if(current_tab == 2) {
                        if(pressed & PSP_CTRL_UP) { current_range_idx++; if(current_range_idx > 4) current_range_idx = 0; force_refresh = 1; }
                        if(pressed & PSP_CTRL_DOWN) { current_range_idx--; if(current_range_idx < 0) current_range_idx = 4; force_refresh = 1; }
                    }
                    if(current_tab == 3) {
                        if(news_reading_mode == 0) {
                            if(pressed & PSP_CTRL_DOWN) { news_cursor++; if(news_cursor >= t3_count) news_cursor = 0; }
                            if(pressed & PSP_CTRL_UP)   { news_cursor--; if(news_cursor < 0) news_cursor = t3_count > 0 ? t3_count - 1 : 0; }
                            if(pressed & PSP_CTRL_CROSS && t3_count > 0) { news_reading_mode = 1; }
                        } else {
                            if((pressed & PSP_CTRL_CIRCLE) || (pressed & PSP_CTRL_CROSS)) { news_reading_mode = 0; }
                        }
                    }
                } 
                else {
                    if(pressed & PSP_CTRL_DOWN) { om_active_field++; if(om_active_field > 6) om_active_field = 0; }
                    if(pressed & PSP_CTRL_UP)   { om_active_field--; if(om_active_field < 0) om_active_field = 6; }
                    
                    if(om_active_field == 0) { 
                        if(pressed & PSP_CTRL_RIGHT) { om_ticker_cursor++; if(om_ticker_cursor > 5) om_ticker_cursor = 0; }
                        if(pressed & PSP_CTRL_LEFT)  { om_ticker_cursor--; if(om_ticker_cursor < 0) om_ticker_cursor = 5; }
                        if(pressed & PSP_CTRL_CROSS) { 
                            om_ticker[om_ticker_cursor]++; 
                            if(om_ticker[om_ticker_cursor] > 'Z') om_ticker[om_ticker_cursor] = ' ';
                            if(om_ticker[om_ticker_cursor] == ' ' + 1) om_ticker[om_ticker_cursor] = 'A'; 
                        }
                        if(pressed & PSP_CTRL_SQUARE) { 
                            om_ticker[om_ticker_cursor]--; 
                            if(om_ticker[om_ticker_cursor] < ' ') om_ticker[om_ticker_cursor] = 'Z';
                            if(om_ticker[om_ticker_cursor] == '@') om_ticker[om_ticker_cursor] = ' '; 
                        }
                    }
                    else if(om_active_field == 1) { 
                        if((pressed & PSP_CTRL_RIGHT) || (pressed & PSP_CTRL_LEFT)) om_side = !om_side;
                    }
                    else if(om_active_field == 2) { 
                        if((pressed & PSP_CTRL_RIGHT) || (pressed & PSP_CTRL_LEFT)) om_qty_type = !om_qty_type;
                    }
                    else if(om_active_field == 3) { 
                        float increment = (om_qty_type == 0) ? 1.0f : 100.0f;
                        if(pressed & PSP_CTRL_RIGHT) om_qty += increment;
                        if(pressed & PSP_CTRL_LEFT)  { om_qty -= increment; if(om_qty < 0.1f) om_qty = 0.1f; }
                    }
                    else if(om_active_field == 4) { 
                        if(pressed & PSP_CTRL_RIGHT) { om_type++; if(om_type > 4) om_type = 0; }
                        if(pressed & PSP_CTRL_LEFT)  { om_type--; if(om_type < 0) om_type = 4; }
                    }
                    else if(om_active_field == 5) { 
                        if(pressed & PSP_CTRL_RIGHT) { om_tif++; if(om_tif > 5) om_tif = 0; }
                        if(pressed & PSP_CTRL_LEFT)  { om_tif--; if(om_tif < 0) om_tif = 5; }
                    }
                    else if(om_active_field == 6) { 
                        if(pressed & PSP_CTRL_CROSS && net_request_active == 0) {
                            char clean_tck[8]; int j=0;
                            for(int i=0; i<6; i++) { if(om_ticker[i] != ' ') clean_tck[j++] = om_ticker[i]; }
                            clean_tck[j] = '\0';
                            
                            snprintf(net_target_endpoint, sizeof(net_target_endpoint), 
                                "/api/order?sym=%s&side=%s&qtype=%s&qty=%.2f&type=%s&tif=%s",
                                clean_tck, om_side == 0 ? "BUY" : "SELL", om_qty_type == 0 ? "SHARES" : "DOLLARS", 
                                om_qty, om_type_strs[om_type], om_tif_strs[om_tif]);
                            
                            net_request_active = 1;
                            active_fetch_tab = 4;
                            strcpy(om_status_msg, "TRANSMITTING TO BROKER...");
                        }
                    }
                }
            }
            
            if(force_refresh && net_request_active == 0) {
                active_fetch_tab = current_tab; 
                char *target_symbol = ticker_list[current_ticker_idx];
                char encoded_sym[64] = ""; int ej = 0;
                for(int i=0; target_symbol[i] != '\0' && i < 60; i++) {
                    if(target_symbol[i] == '/') { encoded_sym[ej++]='%'; encoded_sym[ej++]='2'; encoded_sym[ej++]='F'; }
                    else { encoded_sym[ej++] = target_symbol[i]; }
                }
                encoded_sym[ej] = '\0'; 
                
                if(current_tab == 0) snprintf(net_target_endpoint, sizeof(net_target_endpoint), "/api/main");
                else if(current_tab == 1) snprintf(net_target_endpoint, sizeof(net_target_endpoint), "/api/overview?symbol=%.60s", encoded_sym);
                else if(current_tab == 2) snprintf(net_target_endpoint, sizeof(net_target_endpoint), "/api/chart?symbol=%.60s&range=%.4s", encoded_sym, ranges[current_range_idx]);
                else if(current_tab == 3) snprintf(net_target_endpoint, sizeof(net_target_endpoint), "/api/news?symbol=%.60s", encoded_sym);
                
                if(current_tab != 4) {
                    net_request_active = 1; force_refresh = 0; internal_loading_frame = 0;
                }
            }
            
            if(net_status == 2 || net_status == -1) {
                if (active_fetch_tab == 4) {
                    if(net_status == 2) snprintf(om_status_msg, sizeof(om_status_msg), "%.60s", net_response_buffer);
                    else strcpy(om_status_msg, "ERROR: ROUTING FAILURE");
                    net_status = 0;
                }
                else if (active_fetch_tab == current_tab) {
                    snprintf(api_data, sizeof(api_data), "%.4095s", (char*)net_response_buffer);
                    
                    if(current_tab == 0) {
                        if(net_status == 2) {
                            t0_holdings_count = 0; t0_orders_count = 0;
                            char *save_line; 
                            
                            char *line = strtok_r(api_data, "\r\n", &save_line);
                            if(line) {
                                char *sp; char *p = strtok_r(line, "|", &sp); if(p) snprintf(t0_acc, sizeof(t0_acc), "%.31s", p);
                                char *b = strtok_r(NULL, "|", &sp); if(b) snprintf(t0_buy, sizeof(t0_buy), "%.31s", b);
                            }
                            line = strtok_r(NULL, "\r\n", &save_line);
                            
                            int parsing_orders = 0;
                            while(line != NULL) {
                                if(strncmp(line, "---ORDERS---", 12) == 0) { parsing_orders = 1; }
                                else if(parsing_orders == 0 && t0_holdings_count < 15) { snprintf(t0_holdings[t0_holdings_count++], 64, "%.63s", line); }
                                else if(parsing_orders == 1 && t0_orders_count < 10) { snprintf(t0_orders[t0_orders_count++], 64, "%.63s", line); }
                                line = strtok_r(NULL, "\r\n", &save_line);
                            }
                        }
                    }
                    else if(current_tab == 1) {
                        if(net_status == -1) { strcpy(t1_price, "NET ERR"); }
                        else if(strncmp(api_data, "ERROR", 5) == 0) { 
                            strcpy(t1_price, "API ERR"); snprintf(t1_change, sizeof(t1_change), "%.31s", api_data + 6); 
                        } 
                        else {
                            char *save_line;
                            char *line1 = strtok_r(api_data, "\n", &save_line);
                            char *line2 = strtok_r(NULL, "\n", &save_line);
                            char *line3 = strtok_r(NULL, "\n", &save_line);
                            
                            if(line1) {
                                char *sp;
                                char *p = strtok_r(line1, "|", &sp); if(p) snprintf(t1_price, sizeof(t1_price), "%.31s", p);
                                char *c = strtok_r(NULL, "|", &sp); if(c) snprintf(t1_change, sizeof(t1_change), "%.31s", c);
                                char *pc = strtok_r(NULL, "|", &sp); if(pc) snprintf(t1_pct, sizeof(t1_pct), "%.31s", pc);
                                char *h = strtok_r(NULL, "|", &sp); if(h) snprintf(t1_high, sizeof(t1_high), "%.31s", h);
                                char *l = strtok_r(NULL, "|", &sp); if(l) snprintf(t1_low, sizeof(t1_low), "%.31s", l);
                                char *v = strtok_r(NULL, "|", &sp); if(v) snprintf(t1_vol, sizeof(t1_vol), "%.31s", v);
                            }
                            if(line2) {
                                char *sp;
                                char *pe = strtok_r(line2, "|", &sp); if(pe) snprintf(t1_pe, sizeof(t1_pe), "%.15s", pe);
                                char *pb = strtok_r(NULL, "|", &sp); if(pb) snprintf(t1_pb, sizeof(t1_pb), "%.15s", pb);
                                char *eps = strtok_r(NULL, "|", &sp); if(eps) snprintf(t1_eps, sizeof(t1_eps), "%.15s", eps);
                                char *div = strtok_r(NULL, "|", &sp); if(div) snprintf(t1_div, sizeof(t1_div), "%.15s", div);
                                char *mcap = strtok_r(NULL, "|", &sp); if(mcap) snprintf(t1_mcap, sizeof(t1_mcap), "%.15s", mcap);
                                char *beta = strtok_r(NULL, "|", &sp); if(beta) snprintf(t1_beta, sizeof(t1_beta), "%.15s", beta);
                            }
                            if(line3) {
                                char *sp;
                                char *sec = strtok_r(line3, "|", &sp); if(sec) snprintf(t1_sec, sizeof(t1_sec), "%.31s", sec);
                                char *ind = strtok_r(NULL, "|", &sp); if(ind) snprintf(t1_ind, sizeof(t1_ind), "%.31s", ind);
                            }
                        }
                    }
                    else if(current_tab == 2) {
                        chart_point_count = 0;
                        if(net_status == -1) { strcpy(max_price_str, "NET_ERROR"); }
                        else if(strncmp(api_data, "ERROR", 5) == 0) { strcpy(max_price_str, "API_ERROR"); } 
                        else {
                            char *save_line;
                            
                            char *header = strtok_r(api_data, "\r\n", &save_line);
                            char *d_cp = strtok_r(NULL, "\r\n", &save_line);
                            char *d_sma = strtok_r(NULL, "\r\n", &save_line);
                            char *d_ubb = strtok_r(NULL, "\r\n", &save_line);
                            char *d_lbb = strtok_r(NULL, "\r\n", &save_line);
                            
                            if(header && d_cp && d_sma && d_ubb && d_lbb) {
                                char *sp; char *mx = strtok_r(header, "|", &sp); char *mn = strtok_r(NULL, "|", &sp);
                                if(mx) snprintf(max_price_str, sizeof(max_price_str), "%.63s", mx); 
                                if(mn) snprintf(min_price_str, sizeof(min_price_str), "%.63s", mn); 
                                
                                char *sv_d; char *val; int tmp;
                                val = strtok_r(d_cp, ",", &sv_d); while(val != NULL && chart_point_count < 100) { chart_points[chart_point_count++] = atoi(val); val = strtok_r(NULL, ",", &sv_d); }
                                tmp = 0; val = strtok_r(d_sma, ",", &sv_d); while(val != NULL && tmp < 100) { chart_sma[tmp++] = atoi(val); val = strtok_r(NULL, ",", &sv_d); }
                                tmp = 0; val = strtok_r(d_ubb, ",", &sv_d); while(val != NULL && tmp < 100) { chart_ubb[tmp++] = atoi(val); val = strtok_r(NULL, ",", &sv_d); }
                                tmp = 0; val = strtok_r(d_lbb, ",", &sv_d); while(val != NULL && tmp < 100) { chart_lbb[tmp++] = atoi(val); val = strtok_r(NULL, ",", &sv_d); }
                            }
                        }
                    }
                    else if(current_tab == 3) {
                        if(net_status == 2) {
                            t3_count = 0; 
                            char *save_line;
                            char *line = strtok_r(api_data, "\r\n", &save_line);
                            while(line != NULL && t3_count < 10) {
                                char *sp;
                                char *src = strtok_r(line, "|", &sp); 
                                char *snt = strtok_r(NULL, "|", &sp);
                                char *hdl = strtok_r(NULL, "|", &sp);
                                char *dsc = strtok_r(NULL, "\r\n", &sp);
                                
                                if(src && snt && hdl) {
                                    snprintf(t3_source[t3_count], 16, "%.15s", src);
                                    snprintf(t3_sentiment[t3_count], 8, "%.7s", snt);
                                    snprintf(t3_headline[t3_count], 64, "%.63s", hdl);
                                    if(dsc) snprintf(t3_desc[t3_count], 512, "%.511s", dsc);
                                    else snprintf(t3_desc[t3_count], 512, "No summary available.");
                                    t3_count++;
                                }
                                line = strtok_r(NULL, "\r\n", &save_line);
                            }
                        }
                    }
                } else {
                    force_refresh = 1;
                }
                net_status = 0;
            }
        }
        
        sceGuStart(GU_DIRECT, list); sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT); 
        
        if(app_ready == 1 && net_status == 0 && current_tab == 2 && chart_point_count > 1) {
            int graph_w = 400, graph_h = 160, origin_x = 40, origin_y = 220; 
            float step_x = (float)graph_w / (chart_point_count - 1);
            
            struct Vertex* v_ubb = (struct Vertex*)sceGuGetMemory(chart_point_count * sizeof(struct Vertex));
            struct Vertex* v_lbb = (struct Vertex*)sceGuGetMemory(chart_point_count * sizeof(struct Vertex));
            struct Vertex* v_sma = (struct Vertex*)sceGuGetMemory(chart_point_count * sizeof(struct Vertex));
            struct Vertex* v_cp = (struct Vertex*)sceGuGetMemory(chart_point_count * sizeof(struct Vertex));

            for(int i=0; i<chart_point_count; i++) {
                float px = (float)origin_x + (i * step_x);
                
                v_ubb[i].color = 0xFF442222; v_ubb[i].x = px; v_ubb[i].y = (float)origin_y - ((chart_ubb[i] / 100.0f) * graph_h); v_ubb[i].z = 0.0f;
                v_lbb[i].color = 0xFF442222; v_lbb[i].x = px; v_lbb[i].y = (float)origin_y - ((chart_lbb[i] / 100.0f) * graph_h); v_lbb[i].z = 0.0f;
                v_sma[i].color = 0xFFAAAAAA; v_sma[i].x = px; v_sma[i].y = (float)origin_y - ((chart_sma[i] / 100.0f) * graph_h); v_sma[i].z = 0.0f;
                v_cp[i].color  = 0xFF00FFFF; v_cp[i].x = px;  v_cp[i].y = (float)origin_y - ((chart_points[i] / 100.0f) * graph_h); v_cp[i].z = 0.0f;
            }
            
            sceGuDrawArray(GU_LINE_STRIP, GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D, chart_point_count, 0, v_ubb);
            sceGuDrawArray(GU_LINE_STRIP, GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D, chart_point_count, 0, v_lbb);
            sceGuDrawArray(GU_LINE_STRIP, GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D, chart_point_count, 0, v_sma);
            sceGuDrawArray(GU_LINE_STRIP, GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D, chart_point_count, 0, v_cp);
        }
        sceGuFinish(); sceGuSync(0, 0);
        pspDebugScreenSetOffset(current_draw); pspDebugScreenSetXY(0, 0);
        
        if(app_ready == 0) {
            printCentered(13, "PROJECT SPLOOM: QUANT ENGINE", COLOR_WHITE);
            printCentered(15, "SYNCING PORTFOLIO ASSETS...", COLOR_TEAL);
            internal_loading_frame++;
            if((internal_loading_frame / 15) % 2 == 0) printCentered(17, ">>> FETCHING TICKERS <<<", COLOR_GREEN);
        } 
        else {
            drawTopBar(current_tab);
            char *target_symbol = ticker_list[current_ticker_idx];
            
            if(net_status == 1 && current_tab != 4) {
                internal_loading_frame++;
                if((internal_loading_frame / 20) % 2 == 0) printCentered(15, "DOWNLOADING DATA...", COLOR_TEAL);
            } 
            else {
                if(current_tab == 0) {
                    pspDebugScreenSetTextColor(COLOR_TEAL); pspDebugScreenSetXY(2, 4); pspDebugScreenPrintf("ACCOUNT VALUE:");
                    pspDebugScreenSetTextColor(COLOR_GREEN); pspDebugScreenSetXY(17, 4); pspDebugScreenPrintf("$%s", t0_acc);
                    pspDebugScreenSetTextColor(COLOR_TEAL); pspDebugScreenSetXY(2, 5); pspDebugScreenPrintf("BUYING POWER:");
                    pspDebugScreenSetTextColor(COLOR_WHITE); pspDebugScreenSetXY(17, 5); pspDebugScreenPrintf("$%s", t0_buy);
                    
                    drawFullLine(7, COLOR_TEAL, '-');
                    int y = 8;
                    for (int i=0; i<t0_holdings_count && y<18; i++) {
                        char tmp[64]; snprintf(tmp, 64, "%.63s", t0_holdings[i]);
                        char *sp; char *sym = strtok_r(tmp, "|", &sp); char *qty = strtok_r(NULL, "|", &sp);
                        char *pnl = strtok_r(NULL, "|", &sp); char *weight = strtok_r(NULL, "|", &sp);
                        if(sym) {
                            pspDebugScreenSetTextColor(COLOR_WHITE); pspDebugScreenSetXY(2, y); pspDebugScreenPrintf("%-8s", sym);
                            pspDebugScreenSetXY(11, y); pspDebugScreenPrintf("%-8s", qty ? qty : "0");
                            
                            if(pnl && pnl[0] == '+') pspDebugScreenSetTextColor(COLOR_GREEN); 
                            else if(pnl && pnl[0] == '-') pspDebugScreenSetTextColor(COLOR_RED);
                            pspDebugScreenSetXY(21, y); pspDebugScreenPrintf("$%-8s", pnl ? pnl : "0");
                            
                            pspDebugScreenSetTextColor(COLOR_TEAL); pspDebugScreenSetXY(32, y); pspDebugScreenPrintf("[%s%%]", weight ? weight : "0");
                            y++;
                        }
                    }
                    
                    pspDebugScreenSetTextColor(COLOR_TEAL); pspDebugScreenSetXY(2, 19); pspDebugScreenPrintf("ACTIVE OPEN ORDERS:");
                    drawFullLine(20, COLOR_TEAL, '-');
                    y = 21;
                    for (int i=0; i<t0_orders_count && y<30; i++) {
                        char tmp[64]; snprintf(tmp, 64, "%.63s", t0_orders[i]);
                        char *sp; char *sym = strtok_r(tmp, "|", &sp); char *qty = strtok_r(NULL, "|", &sp);
                        char *side = strtok_r(NULL, "|", &sp); char *stat = strtok_r(NULL, "|", &sp);
                        if(sym) {
                            pspDebugScreenSetTextColor(COLOR_WHITE); pspDebugScreenSetXY(2, y); pspDebugScreenPrintf("%-8s", sym);
                            pspDebugScreenSetXY(12, y); pspDebugScreenPrintf("%-6s", qty ? qty : "0");
                            
                            pspDebugScreenSetTextColor((side && strcmp(side, "BUY")==0) ? COLOR_GREEN : COLOR_RED);
                            pspDebugScreenSetXY(20, y); pspDebugScreenPrintf("%-6s", side ? side : "-");
                            
                            pspDebugScreenSetTextColor(COLOR_TEAL); pspDebugScreenSetXY(28, y); pspDebugScreenPrintf("[%s]", stat ? stat : "PENDING");
                            y++;
                        }
                    }
                }
                else if(current_tab == 1) {
                    char header[128]; snprintf(header, sizeof(header), "<  %.50s SECURITY DESCRIPTION  >", target_symbol);
                    printCentered(3, header, COLOR_WHITE); drawFullLine(4, COLOR_TEAL, '-');
                    
                    pspDebugScreenSetTextColor(COLOR_TEAL); pspDebugScreenSetXY(4, 6); pspDebugScreenPrintf("SECTOR  :");
                    pspDebugScreenSetTextColor(COLOR_WHITE); pspDebugScreenSetXY(14, 6); pspDebugScreenPrintf("%-20s", t1_sec);
                    
                    pspDebugScreenSetTextColor(COLOR_TEAL); pspDebugScreenSetXY(4, 8); pspDebugScreenPrintf("LAST PX :");
                    pspDebugScreenSetTextColor(COLOR_WHITE); pspDebugScreenSetXY(14, 8); pspDebugScreenPrintf("%-8s", t1_price);
                    
                    pspDebugScreenSetTextColor(COLOR_TEAL); pspDebugScreenSetXY(26, 8); pspDebugScreenPrintf("CHANGE :");
                    pspDebugScreenSetTextColor(t1_change[0] == '-' ? COLOR_RED : COLOR_GREEN); 
                    pspDebugScreenSetXY(35, 8); pspDebugScreenPrintf("%s (%s%%)", t1_change, t1_pct);
                    
                    pspDebugScreenSetTextColor(COLOR_TEAL); pspDebugScreenSetXY(4, 10); pspDebugScreenPrintf("5D HIGH :");
                    pspDebugScreenSetTextColor(COLOR_GREEN); pspDebugScreenSetXY(14, 10); pspDebugScreenPrintf("%-8s", t1_high);
                    pspDebugScreenSetTextColor(COLOR_TEAL); pspDebugScreenSetXY(26, 10); pspDebugScreenPrintf("5D LOW :");
                    pspDebugScreenSetTextColor(COLOR_RED); pspDebugScreenSetXY(35, 10); pspDebugScreenPrintf("%-8s", t1_low);
                    
                    pspDebugScreenSetTextColor(COLOR_TEAL); pspDebugScreenSetXY(4, 12); pspDebugScreenPrintf("VOLUME  :");
                    pspDebugScreenSetTextColor(COLOR_WHITE); pspDebugScreenSetXY(14, 12); pspDebugScreenPrintf("%-8s", t1_vol);
                    pspDebugScreenSetTextColor(COLOR_TEAL); pspDebugScreenSetXY(26, 12); pspDebugScreenPrintf("MKT CAP:");
                    pspDebugScreenSetTextColor(COLOR_WHITE); pspDebugScreenSetXY(35, 12); pspDebugScreenPrintf("%-8s", t1_mcap);

                    drawFullLine(14, COLOR_TEAL, '-');
                    
                    pspDebugScreenSetTextColor(COLOR_TEAL); pspDebugScreenSetXY(4, 16); pspDebugScreenPrintf("P/E     :");
                    pspDebugScreenSetTextColor(COLOR_WHITE); pspDebugScreenSetXY(14, 16); pspDebugScreenPrintf("%-6s", t1_pe);
                    
                    pspDebugScreenSetTextColor(COLOR_TEAL); pspDebugScreenSetXY(26, 16); pspDebugScreenPrintf("P/B    :");
                    pspDebugScreenSetTextColor(COLOR_WHITE); pspDebugScreenSetXY(35, 16); pspDebugScreenPrintf("%-6s", t1_pb);

                    pspDebugScreenSetTextColor(COLOR_TEAL); pspDebugScreenSetXY(4, 18); pspDebugScreenPrintf("EPS     :");
                    pspDebugScreenSetTextColor(COLOR_WHITE); pspDebugScreenSetXY(14, 18); pspDebugScreenPrintf("%-6s", t1_eps);
                    
                    pspDebugScreenSetTextColor(COLOR_TEAL); pspDebugScreenSetXY(26, 18); pspDebugScreenPrintf("DIV    :");
                    pspDebugScreenSetTextColor(COLOR_WHITE); pspDebugScreenSetXY(35, 18); pspDebugScreenPrintf("%-6s", t1_div);

                    pspDebugScreenSetTextColor(COLOR_TEAL); pspDebugScreenSetXY(4, 20); pspDebugScreenPrintf("BETA    :");
                    pspDebugScreenSetTextColor(COLOR_WHITE); pspDebugScreenSetXY(14, 20); pspDebugScreenPrintf("%-6s", t1_beta);
                }
                else if(current_tab == 2) {
                    char title[64]; snprintf(title, sizeof(title), "< %.30s  |  ^/v RANGE: %.4s >", target_symbol, ranges[current_range_idx]);
                    printCentered(3, title, COLOR_WHITE);
                    if(chart_point_count > 1) {
                        pspDebugScreenSetTextColor(COLOR_GREEN); pspDebugScreenSetXY(2, 6); pspDebugScreenPrintf("%s", max_price_str);
                        pspDebugScreenSetTextColor(COLOR_RED); pspDebugScreenSetXY(2, 28); pspDebugScreenPrintf("%s", min_price_str);
                    } else {
                        printCentered(15, max_price_str, COLOR_RED); printCentered(17, min_price_str, COLOR_WHITE);
                    }
                } 
                else if(current_tab == 3) {
                    char header[128]; snprintf(header, sizeof(header), "<  %.45s NLP SENTIMENT WIRE  >", target_symbol);
                    printCentered(3, header, COLOR_WHITE); drawFullLine(4, COLOR_TEAL, '-');
                    
                    if(news_reading_mode == 0) {
                        int y = 6;
                        for (int i=0; i<t3_count && y<30; i++) {
                            if(i == news_cursor) {
                                pspDebugScreenSetTextColor(COLOR_WHITE);
                                pspDebugScreenSetXY(0, y); pspDebugScreenPrintf(">");
                                pspDebugScreenSetXY(0, y+1); pspDebugScreenPrintf(">");
                            }
                            
                            pspDebugScreenSetTextColor(COLOR_TEAL); pspDebugScreenSetXY(2, y); pspDebugScreenPrintf("[%s]", t3_source[i]);
                            
                            u32 text_color = COLOR_WHITE;
                            if(t3_sentiment[i][1] == '+') text_color = COLOR_GREEN;
                            else if(t3_sentiment[i][1] == '-') text_color = COLOR_RED;
                            
                            pspDebugScreenSetTextColor(text_color); 
                            pspDebugScreenSetXY(2, y+1); pspDebugScreenPrintf("%s %s", t3_sentiment[i], t3_headline[i]);
                            y += 3;
                        }
                    } else {
                        pspDebugScreenSetTextColor(COLOR_TEAL); pspDebugScreenSetXY(2, 6); pspDebugScreenPrintf("[%s]", t3_source[news_cursor]);
                        
                        u32 text_color = COLOR_WHITE;
                        if(t3_sentiment[news_cursor][1] == '+') text_color = COLOR_GREEN;
                        else if(t3_sentiment[news_cursor][1] == '-') text_color = COLOR_RED;
                        
                        pspDebugScreenSetTextColor(text_color); 
                        pspDebugScreenSetXY(2, 7); pspDebugScreenPrintf("%s %s", t3_sentiment[news_cursor], t3_headline[news_cursor]);
                        drawFullLine(9, COLOR_TEAL, '-');
                        
                        pspDebugScreenSetTextColor(COLOR_WHITE);
                        char *desc = t3_desc[news_cursor];
                        int len = strlen(desc);
                        int c_x = 2, c_y = 11;
                        char word[64]; int w_idx = 0;
                        
                        for(int i=0; i<=len; i++) {
                            if(desc[i] == ' ' || desc[i] == '\0') {
                                word[w_idx] = '\0';
                                if(c_x + w_idx > 66) { c_x = 2; c_y++; }
                                pspDebugScreenSetXY(c_x, c_y); pspDebugScreenPrintf("%s", word);
                                c_x += w_idx + 1;
                                w_idx = 0;
                                if(c_y > 32) break; 
                            } else {
                                if(w_idx < 63) word[w_idx++] = desc[i];
                            }
                        }
                        
                        printCentered(32, ">>> PRESS [O] TO RETURN TO HEADLINES <<<", COLOR_TEAL);
                    }
                }
                else if(current_tab == 4) {
                    printCentered(3, "< ORDER MANAGEMENT SYSTEM >", COLOR_WHITE); drawFullLine(4, COLOR_TEAL, '-');
                    
                    pspDebugScreenSetTextColor(om_active_field == 0 ? COLOR_GREEN : COLOR_TEAL);
                    pspDebugScreenSetXY(10, 6); pspDebugScreenPrintf("TICKER : ");
                    for(int i=0; i<6; i++) {
                        if(om_active_field == 0 && om_ticker_cursor == i) {
                            pspDebugScreenSetTextColor(COLOR_GREEN); pspDebugScreenPrintf("[%c]", om_ticker[i]);
                        } else {
                            pspDebugScreenSetTextColor(COLOR_WHITE); pspDebugScreenPrintf(" %c ", om_ticker[i]);
                        }
                    }
                    if(om_active_field == 0) { pspDebugScreenSetXY(40, 6); pspDebugScreenSetTextColor(COLOR_TEAL); pspDebugScreenPrintf("<- [X] Up [SQ] Dwn"); }
                    
                    pspDebugScreenSetTextColor(om_active_field == 1 ? COLOR_GREEN : COLOR_TEAL);
                    pspDebugScreenSetXY(10, 8); pspDebugScreenPrintf("ACTION : ");
                    pspDebugScreenSetTextColor(om_side == 0 ? COLOR_GREEN : COLOR_RED);
                    if(om_active_field == 1) pspDebugScreenPrintf("[ %s ]", om_side == 0 ? "BUY" : "SELL");
                    else pspDebugScreenPrintf("  %s  ", om_side == 0 ? "BUY" : "SELL");
                    
                    pspDebugScreenSetTextColor(om_active_field == 2 ? COLOR_GREEN : COLOR_TEAL);
                    pspDebugScreenSetXY(10, 10); pspDebugScreenPrintf("METHOD : ");
                    pspDebugScreenSetTextColor(COLOR_WHITE);
                    if(om_active_field == 2) pspDebugScreenPrintf("[ %s ]", om_qty_type == 0 ? "SHARES" : "DOLLARS");
                    else pspDebugScreenPrintf("  %s  ", om_qty_type == 0 ? "SHARES" : "DOLLARS");
                    
                    pspDebugScreenSetTextColor(om_active_field == 3 ? COLOR_GREEN : COLOR_TEAL);
                    pspDebugScreenSetXY(10, 12); pspDebugScreenPrintf("AMOUNT : ");
                    pspDebugScreenSetTextColor(COLOR_WHITE);
                    if(om_active_field == 3) pspDebugScreenPrintf("[ %.2f ]", om_qty);
                    else pspDebugScreenPrintf("  %.2f  ", om_qty);
                    
                    pspDebugScreenSetTextColor(om_active_field == 4 ? COLOR_GREEN : COLOR_TEAL);
                    pspDebugScreenSetXY(10, 14); pspDebugScreenPrintf("TYPE   : ");
                    pspDebugScreenSetTextColor(COLOR_WHITE);
                    if(om_active_field == 4) pspDebugScreenPrintf("[ %s ]", om_type_strs[om_type]);
                    else pspDebugScreenPrintf("  %s  ", om_type_strs[om_type]);
                    
                    pspDebugScreenSetTextColor(om_active_field == 5 ? COLOR_GREEN : COLOR_TEAL);
                    pspDebugScreenSetXY(10, 16); pspDebugScreenPrintf("T.I.F  : ");
                    pspDebugScreenSetTextColor(COLOR_WHITE);
                    if(om_active_field == 5) pspDebugScreenPrintf("[ %s ]", om_tif_strs[om_tif]);
                    else pspDebugScreenPrintf("  %s  ", om_tif_strs[om_tif]);
                    
                    if(om_active_field == 6) {
                        pspDebugScreenSetTextColor(COLOR_GREEN); pspDebugScreenSetXY(20, 20); pspDebugScreenPrintf(">> [ PRESS X TO SUBMIT ORDER ] <<");
                    } else {
                        pspDebugScreenSetTextColor(COLOR_TEAL); pspDebugScreenSetXY(20, 20); pspDebugScreenPrintf("     EXECUTE ORDER (SUBMIT)      ");
                    }
                    
                    printCentered(24, om_status_msg, strncmp(om_status_msg, "ERROR", 5) == 0 ? COLOR_RED : COLOR_WHITE);
                }
            }
        }
        sceDisplayWaitVblankStart(); current_draw = (int)sceGuSwapBuffers(); old_pad = pad;
    }

shutdown:
    is_running = 0;
    sceKernelDelayThread(200000);
    sceGuTerm(); 
    sceKernelExitGame(); 
    return 0;
}