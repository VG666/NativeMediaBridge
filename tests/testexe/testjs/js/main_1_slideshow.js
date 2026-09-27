var http = require('http');
function sleep(delay) {
    var start = (new Date()).getTime();
    while ((new Date()).getTime() - start < delay) {
        // 使用  continue 实现；
        continue;
    }
}
req = http.get('http://kuwo.cn', function (req, res) {
    global.html = '';
    req.on('data', function (data) {
        global.html += data;
    });
    req.on('end', function () {
        var i
        global.html = global.html
            .split('<div class="swiper-wrapper" data-v-6facd977>')[1]
            .split('<div class="swiper-pagination"')[0]
            .split("swiper-slide").splice(2)
        global.slideshow = Array();
        for (i in global.html) {
            global.slideshow.push([
                global.html[i]
                    .split('<a href="')[1]
                    .split('"')[0]
                    .split("/")[4],
                global.html[i]
                    .split('<img src="')[1]
                    .split('"')[0]
            ]);
        }
        global.slideshow_go_play = true;
        slideshow_right();
    });
});
global.slideshow_go_play = false
global.slideshow_i = -1

global.order = [
    [1, 2, 3],
    [2, 3, 4],
    [3, 4, 1],
    [4, 1, 2]
]

function slideshow_right() {
    if (global.slideshow_go_play) {
        global.slideshow_i = global.slideshow_i + 1
        slideshow_i = global.slideshow_i
        img_list = global.slideshow
        order = global.order[slideshow_i]
        document.getElementById("slideshow_1").src =
            img_list[order[0] - 1][1];
        document.getElementById("slideshow_2").src =
            img_list[order[1] - 1][1];
        document.getElementById("slideshow_3").src =
            img_list[order[2] - 1][1];
        if (slideshow_i == 3) {
            global.slideshow_i = -1;
        }
    }
}

function slideshow_left() {
    if (global.slideshow_go_play) {
        global.slideshow_i = global.slideshow_i - 1;
        slideshow_i = global.slideshow_i
        img_list = global.slideshow;
        if (slideshow_i == -2) {
            global.slideshow_i = 3;
        }
        if (slideshow_i == -1) {
            global.slideshow_i = 3;
        }
        order = global.order[slideshow_i]
        document.getElementById("slideshow_1").src =
            img_list[order[0] - 1][1];
        document.getElementById("slideshow_2").src =
            img_list[order[1] - 1][1];
    }
}
setInterval(function () {
    slideshow_right()
    document.getElementById("slideshow_1").style.opacity = 1;
    document.getElementById("slideshow_2").style.opacity = 1;
    document.getElementById("slideshow_3").style.opacity = 1;
    document.getElementById("slideshow_left").style.opacity = 0.5;
    document.getElementById("slideshow_right").style.opacity = 0.5;
    document.getElementById("slideshow_control").style.opacity = 0.8;
}, 3000)

function slideshow_control_i(n) {
    if (global.slideshow_go_play) {
        global.slideshow_i = n - 2
        slideshow_i = global.slideshow_i
        order = global.order[slideshow_i]
        if (slideshow_i == -1) {
            order = global.order[3]
        }
        console.log(order, slideshow_i)
        document.getElementById("slideshow_1").src =
            img_list[order[0] - 1][1];
        document.getElementById("slideshow_2").src =
            img_list[order[1] - 1][1];
        document.getElementById("slideshow_3").src =
            img_list[order[2] - 1][1];
        if (slideshow_i == 3) {
            global.slideshow_i = -1;
        }
    }
}
setInterval(function () {
    document.getElementById("slideshow_left").style.top =
        document.getElementById("main-tab").offsetHeight +
        (document.getElementById("slideshow").offsetHeight * 0.5) + 5;

    document.getElementById("slideshow_right").style.top = document.getElementById("slideshow_left").style.top;

    document.getElementById("slideshow_right").style.right = document.getElementById("slideshow_left").offsetLeft;

    document.getElementById("slideshow_control").style.top =
        document.getElementById("main-tab").offsetHeight +
        (document.getElementById("slideshow").offsetHeight - 10);

    document.getElementById("slideshow").style.height =
        (
            document.body.offsetHeight -
            document.getElementById("main-tab").offsetHeight
        ) * (30 / 100)

    //document.getElementById("Playlist_recommended").style.height = (document.getElementById("Playlist_recommended").offsetWidth - 52) / 5;

    for (i = 1; i < 5; i++) {
        document.getElementById("slideshow_control_" + i.toString()).innerHTML = "fiber_manual_record";
    }
    if (global.slideshow_i < 0) {
        document.getElementById("slideshow_control_1").innerHTML = "adjust";
    } else {
        document.getElementById("slideshow_control_" + (global.slideshow_i + 2).toString()).innerHTML = "adjust";
    }
}, 100)