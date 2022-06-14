package event

import (
	"hades-ebpf/userspace/decoder"
	"strings"

	manager "github.com/ehids/ebpfmanager"
)

var DefaultExecve = &Execve{}

var _ decoder.Event = (*Execve)(nil)

type Execve struct {
	decoder.BasicEvent `json:"-"`
	Exe                string `json:"-"`
	Cwd                string `json:"cwd"`
	TTYName            string `json:"tty_name"`
	Stdin              string `json:"stdin"`
	Stdout             string `json:"stdout"`
	RemotePort         string `json:"remote_port"`
	RemoteAddr         string `json:"remote_addr"`
	PidTree            string `json:"pid_tree"`
	Cmdline            string `json:"cmdline"`
	PrivEscalation     uint8  `json:"priv_esca"`
	SSHConnection      string `json:"ssh_connection"`
	LDPreload          string `json:"ld_preload"`
	LDLibraryPath      string `json:"ld_library_path"`
}

func (Execve) ID() uint32 {
	return 700
}

func (Execve) String() string {
	return "execve"
}

func (e *Execve) GetExe() string {
	return e.Exe
}

func (e *Execve) Parse() (err error) {
	if e.Exe, err = decoder.DefaultDecoder.DecodeString(); err != nil {
		return
	}
	if e.Cwd, err = decoder.DefaultDecoder.DecodeString(); err != nil {
		return
	}
	if e.TTYName, err = decoder.DefaultDecoder.DecodeString(); err != nil {
		return
	}
	if e.Stdin, err = decoder.DefaultDecoder.DecodeString(); err != nil {
		return
	}
	if e.Stdout, err = decoder.DefaultDecoder.DecodeString(); err != nil {
		return
	}
	if e.RemotePort, e.RemoteAddr, err = decoder.DefaultDecoder.DecodeRemoteAddr(); err != nil {
		return
	}
	if e.PidTree, err = decoder.DefaultDecoder.DecodePidTree(&e.PrivEscalation); err != nil {
		return
	}
	var strArr []string
	if strArr, err = decoder.DefaultDecoder.DecodeStrArray(); err != nil {
		return
	}
	e.Cmdline = strings.Join(strArr, " ")

	envs := make([]string, 0, 3)
	if envs, err = decoder.DefaultDecoder.DecodeStrArray(); err != nil {
		return
	}
	for _, env := range envs {
		if strings.HasPrefix(env, "SSH_CONNECTION=") {
			e.SSHConnection = strings.TrimLeft(env, "SSH_CONNECTION=")
		} else if strings.HasPrefix(env, "LD_PRELOAD=") {
			e.LDPreload = strings.TrimLeft(env, "LD_PRELOAD=")
		}
	}
	if len(e.SSHConnection) == 0 {
		e.SSHConnection = "-1"
	}
	if len(e.LDPreload) == 0 {
		e.LDPreload = "-1"
	}
	if len(e.LDLibraryPath) == 0 {
		e.LDLibraryPath = "-1"
	}
	return
}

func (Execve) GetProbe() []*manager.Probe {
	return []*manager.Probe{
		{
			UID:              "KretprobeSysExecve",
			Section:          "kretprobe/sys_execve",
			EbpfFuncName:     "kretprobe_sys_execve",
			AttachToFuncName: "execve",
		},
	}
}

func init() {
	decoder.Regist(DefaultExecve)
}
