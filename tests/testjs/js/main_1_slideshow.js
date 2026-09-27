window.slideshow_go_play = false
window.slideshow_i = 2


function go_play(slideshow) {
    var i
    window.slideshow = eval(slideshow);
    window.slideshow_go_play = true;
}

function slideshow_right() {
    if (window.slideshow_go_play) {
        order = [
            [1, 2, 3],
            [2, 3, 4],
            [3, 4, 1],
            [4, 1, 2]
        ]
        window.slideshow_i = window.slideshow_i + 1;
        slideshow_i = window.slideshow_i;
        img_list = window.slideshow;
        order = order[slideshow_i];
        console.log(slideshow_i, order)
        console.log(img_list[order[0] - 1][1]);
        console.log(img_list[order[1] - 1][1]);
        console.log(img_list[order[2] - 1][1]);
        document.getElementById("slideshow_1").src =
            img_list[order[0] - 1][1];
        document.getElementById("slideshow_2").src =
            img_list[order[1] - 1][1];
        document.getElementById("slideshow_3").src =
            img_list[order[2] - 1][1];
        if (slideshow_i == 3) {
            window.slideshow_i = -1;
        }
    }
}

function slideshow_left() {
    if (window.slideshow_go_play) {
        order = [
            [1, 2, 3],
            [2, 3, 4],
            [3, 4, 1],
            [4, 1, 2]
        ]
        window.slideshow_i = window.slideshow_i - 1;
        slideshow_i = window.slideshow_i
        img_list = window.slideshow;
        if (slideshow_i == -2) {
            window.slideshow_i = 3;
        }
        if (slideshow_i == -1) {
            window.slideshow_i = 3;
        }
        order = order[slideshow_i]
        document.getElementById("slideshow_1").src =
            img_list[order[0] - 1][1];
        document.getElementById("slideshow_2").src =
            img_list[order[1] - 1][1];
    }
}
setInterval(function () {
    document.getElementById("slideshow_1").style.opacity = 1;
    document.getElementById("slideshow_2").style.opacity = 1;
    document.getElementById("slideshow_3").style.opacity = 1;
    document.getElementById("slideshow_left").style.opacity = 0.5;
    document.getElementById("slideshow_right").style.opacity = 0.5;
    document.getElementById("slideshow_control").style.opacity = 0.8;
    slideshow_right()
}, 3000)

function slideshow_control_i(n) {
    if (window.slideshow_go_play) {
        order = [
            [1, 2, 3],
            [2, 3, 4],
            [3, 4, 1],
            [4, 1, 2]
        ]
        window.slideshow_i = n - 2;
        slideshow_i = window.slideshow_i
        order = order[slideshow_i]
        if (slideshow_i == -1) {
            order = [4, 1, 2];
        }
        console.log(order, slideshow_i)
        document.getElementById("slideshow_1").src =
            img_list[order[0] - 1][1];
        document.getElementById("slideshow_2").src =
            img_list[order[1] - 1][1];
        document.getElementById("slideshow_3").src =
            img_list[order[2] - 1][1];
        if (slideshow_i == 3) {
            window.slideshow_i = -1;
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
    if (window.slideshow_i < 0) {
        document.getElementById("slideshow_control_1").innerHTML = "adjust";
    } else {
        document.getElementById("slideshow_control_" + (window.slideshow_i + 2).toString()).innerHTML = "adjust";
    }
}, 100)
window.parent.call_py_func("kuwo_slideshow");