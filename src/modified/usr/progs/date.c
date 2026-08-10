// #include "shell.h"
// #include "syscall.h"
// #include "string.h"
// #include <stdint.h>

// void main(int argc, char* argv[]){
//     int rtc_fd = _open(-1, "dev/rtc0");
//     if(rtc_fd < 0){
//         _write(CONSOLEOUT, "Could not open RTC device\n", 26);
//         _exit();
//     }
//     //Probably did this function in the grossest way possible
    
//     uint64_t time_ns;
//     _read(rtc_fd, &time_ns, sizeof(time_ns));
//     _close(rtc_fd);
//     uint64_t time_s = time_ns/1000/1000/1000; //now in seconds
//     uint64_t years = 1970 + (time_s - 13*(527040-525600)*60) / (525600 * 60); //including leap years
//     uint64_t curr = time_s - ((years-1970)*525600*60);
//     uint64_t days = curr / 60 / 60 / 24 + 1;
//     // uint64_t weeks = 52 - days % 52;
//     uint64_t months = 0;
//     int cur_day = days;
//     while(cur_day > 0 && months < 12){
//             if(months < 7){
//                 if(months == 1)
//                     cur_day -= 28;
//                 else if(months % 2)
//                     cur_day -= 30;
//                 else{
//                     cur_day -= 31;
//                 }
//             }
//             else{
//                 if(months % 2)
//                     cur_day -= 31;
//                 else{
//                     cur_day -= 30;
//                 }
//             }
//             months++;
//     }

//     if(months < 8){
//             if(months == 2)
//                 cur_day += 28;
//             else if(months % 2)
//                 cur_day += 31;
//             else{
//                 cur_day += 30;
//             }
//         }
//         else{
//             if(months % 2)
//                 cur_day += 30;
//             else{
//                 cur_day += 31;
//             }
//         }

//     uint64_t time = (curr - ((days-1) * 60 * 60 * 24));
//     int hours = time/3600;
//     int minutes = (time - hours*3600) / 60;
//     int seconds = (time - hours*3600) % 60;
//     char *day, *month; //, year, hour;
//     switch(months){
//         case 1:
//             month = "Jan";
//             break;
//         case 2:
//             month = "Feb";
//             break;
//         case 3:
//             month = "Mar";
//             break;
//         case 4:
//             month = "Apr";
//             break;
//         case 5:
//             month = "May";
//             break;
//         case 6:
//             month = "Jun";
//             break;
//         case 7:
//             month = "Jul";
//             break;
//         case 8:
//             month = "Aug";
//             break;
//         case 9:
//             month = "Sep";
//             break;
//         case 10:
//             month = "Oct";
//             break;
//         case 11:
//             month = "Nov";
//             break;
//         case 12:
//             month = "Dec";
//             break;
//         default:
//             month = "Jan";
//             break;
//     }
//     char temp[3];
//     switch(cur_day){
//         case 1:
//             day = "01";
//             break;
//         case 2:
//             day = "02";
//             break;
//         case 3:
//             day = "03";
//             break;
//         case 4:
//             day = "04";
//             break;
//         case 5:
//             day = "05";
//             break;
//         case 6:
//             day = "06";
//             break;
//         case 7:
//             day = "07";
//             break;
//         case 8:
//             day = "08";
//             break;
//         case 9:
//             day = "09";
//             break;
//         default:
//             snprintf(temp, sizeof(temp), "%d", cur_day);
//             day = temp;
//     }
//     char *hour, *minute, *second;
//     char temp_h[3];
//     if(hours < 10){
//         snprintf(temp_h, sizeof(temp), "0%d", hours);
//         hour = temp_h;
//     }
//     else{
//         snprintf(temp_h, sizeof(temp), "%d", hours);
//         hour = temp_h;
//     }
    
//     char temp_m[3];
//     if(minutes < 10){
//         snprintf(temp_m, sizeof(temp), "0%d", minutes);
//         minute = temp_m;
//     }
//     else{
//         snprintf(temp_m, sizeof(temp), "%d", minutes);
//         minute = temp_m;
//     }
    
//     char temp_s[3];
//     if(seconds < 10){
//         snprintf(temp_s, sizeof(temp), "0%d", seconds);
//         second = temp_s;
//     }
//     else{
//         snprintf(temp_s, sizeof(temp), "%d", seconds);
//         second = temp_s;
//     }

//     static char date[50];
//     snprintf(date, sizeof(date), "%s %s %d %s:%s:%s\n", day, month, years, hour, minute, second);
//     _write(STDOUT, date, strlen(date));
//     _exit();
// }
#include "shell.h"
#include "syscall.h"
#include "string.h"
#include <stdint.h>

static int is_leap_year(int year) {
    if ((year % 400) == 0) return 1;
    if ((year % 100) == 0) return 0;
    return (year % 4) == 0;
}

static int days_in_year(int year) {
    return is_leap_year(year) ? 366 : 365;
}

static int days_in_month(int year, int month_index) {
    static const int month_days[12] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
    };

    if (month_index == 1) {  // February
        return month_days[month_index] + is_leap_year(year);
    }

    return month_days[month_index];
}

void main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    int rtc_fd = _open(-1, "dev/rtc0");
    if (rtc_fd < 0) {
        _write(CONSOLEOUT, "Could not open RTC device\r\n", 27);
        _exit();
    }

    uint64_t time_ns = 0;
    int rc = _read(rtc_fd, &time_ns, sizeof(time_ns));
    _close(rtc_fd);

    if (rc < (int)sizeof(time_ns)) {
        _write(CONSOLEOUT, "Could not read RTC device\r\n", 27);
        _exit();
    }

    uint64_t total_seconds = time_ns / 1000000000ULL;
    uint64_t total_days = total_seconds / 86400ULL;
    uint64_t seconds_of_day = total_seconds % 86400ULL;

    int year = 1970;
    while (total_days >= (uint64_t)days_in_year(year)) {
        total_days -= (uint64_t)days_in_year(year);
        year++;
    }

    int month = 0;
    while (month < 12 && total_days >= (uint64_t)days_in_month(year, month)) {
        total_days -= (uint64_t)days_in_month(year, month);
        month++;
    }

    int day = (int)total_days + 1;
    int hour = (int)(seconds_of_day / 3600ULL);
    int minute = (int)((seconds_of_day % 3600ULL) / 60ULL);
    int second = (int)(seconds_of_day % 60ULL);

    static const char *month_names[12] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };

    char out[64];
    snprintf(out, sizeof(out), "%02d %s %d %02d:%02d:%02d UTC\r\n",
             day, month_names[month], year, hour, minute, second);

    _write(STDOUT, out, strlen(out));
    _exit();
}