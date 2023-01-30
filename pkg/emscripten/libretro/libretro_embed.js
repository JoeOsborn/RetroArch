/**
 * RetroArch Web Player
 *
 * This provides the basic JavaScript for the RetroArch web player.
 */
const core = "fceumm";
const content_folder = "/content/";
const content = "bfight.nes";
const entrySlot = 0;
var contentBase = content.substr(0,content.lastIndexOf("."));

var BrowserFS = BrowserFS;
var afs;
var initializationCount = 0;

function cleanupStorage()
{
   localStorage.clear();
   document.getElementById("btnClean").disabled = true;
}

function fsInit()
{
   $('#icnLocal').removeClass('fa-globe');
   $('#icnLocal').addClass('fa-spinner fa-spin');
   afs = new BrowserFS.FileSystem.InMemory();
   setupFileSystem("browser");
   appInitialized();
}

function appInitialized()
{
     /* Need to wait for both the file system and the wasm runtime 
        to complete before enabling the Run button. */
     initializationCount++;
     if (initializationCount == 2)
     {
         preLoadingComplete();
     }
 }

function preLoadingComplete()
{
   /* Make the Preview image clickable to start RetroArch. */
   $('.webplayer-preview').addClass('loaded').click(function () {
      startRetroArch();
      return false;
  });
}

function setupFileSystem(backend)
{
   /* create a mountable filesystem that will server as a root
      mountpoint for browserfs */
   var mfs =  new BrowserFS.FileSystem.MountableFileSystem();

   /* create an XmlHttpRequest filesystem for the bundled data */
   var xfs1 =  new BrowserFS.FileSystem.XmlHttpRequest
      (".index-xhr", "assets/frontend/bundle/");
   /* create an XmlHttpRequest filesystem for core assets */
    // var xfs2 =  new BrowserFS.FileSystem.XmlHttpRequest
    // ([], "assets/cores/");
    var xfs_content_files = {"retroarch.cfg":null};
    xfs_content_files[content] = null;
    if (entrySlot != 0) {
        xfs_content_files["entry_state"] = null;
    }
    var xfs_content = new BrowserFS.FileSystem.XmlHttpRequest(xfs_content_files, content_folder);

    console.log("WEBPLAYER: initializing filesystem: " + backend);
    mfs.mount('/home/web_user/retroarch/userdata', afs);

    mfs.mount('/home/web_user/retroarch/bundle', xfs1);
    // mfs.mount('/home/web_user/retroarch/userdata/content/downloads', xfs2);
    mfs.mount('/home/web_user/content', xfs_content);
    BrowserFS.initialize(mfs);
    var BFS = new BrowserFS.EmscriptenFS();
    FS.mount(BFS, {root: '/home'}, '/home');

    if (entrySlot != 0) {
        FS.mkdir("/home/web_user/retroarch/userdata/states");
        copyFile("/home/web_user/content/entry_state",
                 "/home/web_user/retroarch/userdata/states/"+contentBase+".state1.entry");
    }
    copyFile("/home/web_user/content/retroarch.cfg", "/home/web_user/retroarch/userdata/retroarch.cfg");
    console.log("WEBPLAYER: " + backend + " filesystem initialization successful");
}

function copyFile(from, to) {
    var buf = FS.readFile(from);
    FS.writeFile(to, buf);
}

function startRetroArch()
{
   $('.webplayer').show();
   $('.webplayer-preview').hide();

   $('#btnMenu').removeClass('disabled');
   $('#btnAdd').removeClass('disabled');
   $('#btnRom').removeClass('disabled');

   document.getElementById("btnAdd").disabled = false;
   document.getElementById("btnRom").disabled = false;
   document.getElementById("btnMenu").disabled = false;

   Module['callMain'](Module['arguments']);
   Module['resumeMainLoop']();
   document.getElementById('canvas').focus();
}

function selectFiles(files)
{
   $('#btnAdd').addClass('disabled');
   $('#icnAdd').removeClass('fa-plus');
   $('#icnAdd').addClass('fa-spinner spinning');
   var count = files.length;

   for (var i = 0; i < count; i++)
   {
      filereader = new FileReader();
      filereader.file_name = files[i].name;
      filereader.readAsArrayBuffer(files[i]);
      filereader.onload = function(){uploadData(this.result, this.file_name);};
      filereader.onloadend = function(evt)
      {
         console.log("WEBPLAYER: file: " + this.file_name + " upload complete");
         if (evt.target.readyState == FileReader.DONE)
         {
            $('#btnAdd').removeClass('disabled');
            $('#icnAdd').removeClass('fa-spinner spinning');
            $('#icnAdd').addClass('fa-plus');
         }
      };
   }
}

function uploadData(data,name)
{
   var dataView = new Uint8Array(data);
   FS.createDataFile('/', name, dataView, true, false);

   var data = FS.readFile(name,{ encoding: 'binary' });
   FS.writeFile('/home/web_user/retroarch/userdata/content/' + name, data ,{ encoding: 'binary' });
   FS.unlink(name);
}

var Module =
{
  noInitialRun: true,
    arguments: ["-v", "-R", "/home/web_user/retroarch/userdata/movie.bsv", "-e", entrySlot.toString(), "/home/web_user/content/"+content],
  preRun: [],
  postRun: [],
  onRuntimeInitialized: function()
  {
     appInitialized();
  },
  print: function(text)
  {
     console.log(text);
  },
  printErr: function(text)
  {
     console.log(text);
  },
  canvas: document.getElementById('canvas'),
  totalDependencies: 0,
  monitorRunDependencies: function(left)
  {
     this.totalDependencies = Math.max(this.totalDependencies, left);
  }
};

// When the browser has loaded everything.
$(function() {
   // Enable all available ToolTips.
   $('.tooltip-enable').tooltip({
      placement: 'right'
   });

   /**
    * Attempt to disable some default browser keys.
    */
	var keys = {
    9: "tab",
    13: "enter",
    16: "shift",
    18: "alt",
    27: "esc",
    33: "rePag",
    34: "avPag",
    35: "end",
    36: "home",
    37: "left",
    38: "up",
    39: "right",
    40: "down",
    112: "F1",
    113: "F2",
    114: "F3",
    115: "F4",
    116: "F5",
    117: "F6",
    118: "F7",
    119: "F8",
    120: "F9",
    121: "F10",
    122: "F11",
    123: "F12"
  };
	window.addEventListener('keydown', function (e) {
    if (keys[e.which]) {
      e.preventDefault();
    }
  });

   if (!core) {
      core = 'gambatte';
   }
   // Load the Core's related JavaScript.
   $.getScript(core + '_libretro.js', function ()
   {
      fsInit();
   });
 });

function keyPress(k)
{
   kp(k, "keydown");
   setTimeout(function(){kp(k, "keyup")}, 50);
}

kp = function(k, event) {
   var oEvent = new KeyboardEvent(event, { code: k });

   document.dispatchEvent(oEvent);
   document.getElementById('canvas').focus();
}


function downloadFiles(path) {
    const root = "/home/web_user/retroarch/userdata";
    
}
