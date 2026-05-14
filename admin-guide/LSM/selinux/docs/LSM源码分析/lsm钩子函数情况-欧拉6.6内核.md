# security hook: security_audit_rule_free

kernel/auditfilter.c:1

```shell
kernel/auditfilter.c-	case AUDIT_OBJ_LEV_LOW:
kernel/auditfilter.c-	case AUDIT_OBJ_LEV_HIGH:
kernel/auditfilter.c-		kfree(f->lsm_str);
kernel/auditfilter.c:		security_audit_rule_free(f->lsm_rule);
kernel/auditfilter.c-	}
kernel/auditfilter.c-}
kernel/auditfilter.c-
```

---

# security hook: security_audit_rule_init

kernel/auditfilter.c:2

```shell
kernel/auditfilter.c-			}
kernel/auditfilter.c-			entry->rule.buflen += f_val;
kernel/auditfilter.c-			f->lsm_str = str;
kernel/auditfilter.c:			err = security_audit_rule_init(f->type, f->op, str,
kernel/auditfilter.c-						       (void **)&f->lsm_rule,
kernel/auditfilter.c-						       GFP_KERNEL);
kernel/auditfilter.c-			/* Keep currently invalid fields around in case they
--
kernel/auditfilter.c-	df->lsm_str = lsm_str;
kernel/auditfilter.c-
kernel/auditfilter.c-	/* our own (refreshed) copy of lsm_rule */
kernel/auditfilter.c:	ret = security_audit_rule_init(df->type, df->op, df->lsm_str,
kernel/auditfilter.c-				       (void **)&df->lsm_rule, GFP_KERNEL);
kernel/auditfilter.c-	/* Keep currently invalid fields around in case they
kernel/auditfilter.c-	 * become valid after a policy reload. */
```

---

# security hook: security_audit_rule_known

kernel/auditfilter.c:1

```shell
kernel/auditfilter.c-	struct audit_entry *nentry;
kernel/auditfilter.c-	int err = 0;
kernel/auditfilter.c-
kernel/auditfilter.c:	if (!security_audit_rule_known(r))
kernel/auditfilter.c-		return 0;
kernel/auditfilter.c-
kernel/auditfilter.c-	nentry = audit_dupe_rule(r);
```

---

# security hook: security_audit_rule_match

kernel/auditsc.c:4
kernel/auditfilter.c:1

```shell
kernel/auditsc.c-					security_current_getsecid_subj(&sid);
kernel/auditsc.c-					need_sid = 0;
kernel/auditsc.c-				}
kernel/auditsc.c:				result = security_audit_rule_match(sid, f->type,
kernel/auditsc.c-								   f->op,
kernel/auditsc.c-								   f->lsm_rule);
kernel/auditsc.c-			}
--
kernel/auditsc.c-			if (f->lsm_rule) {
kernel/auditsc.c-				/* Find files that match */
kernel/auditsc.c-				if (name) {
kernel/auditsc.c:					result = security_audit_rule_match(
kernel/auditsc.c-								name->osid,
kernel/auditsc.c-								f->type,
kernel/auditsc.c-								f->op,
kernel/auditsc.c-								f->lsm_rule);
kernel/auditsc.c-				} else if (ctx) {
kernel/auditsc.c-					list_for_each_entry(n, &ctx->names_list, list) {
kernel/auditsc.c:						if (security_audit_rule_match(
kernel/auditsc.c-								n->osid,
kernel/auditsc.c-								f->type,
kernel/auditsc.c-								f->op,
--
kernel/auditsc.c-				/* Find ipc objects that match */
kernel/auditsc.c-				if (!ctx || ctx->type != AUDIT_IPC)
kernel/auditsc.c-					break;
kernel/auditsc.c:				if (security_audit_rule_match(ctx->ipc.osid,
kernel/auditsc.c-							      f->type, f->op,
kernel/auditsc.c-							      f->lsm_rule))
kernel/auditsc.c-					++result;
--
kernel/auditfilter.c-			case AUDIT_SUBJ_CLR:
kernel/auditfilter.c-				if (f->lsm_rule) {
kernel/auditfilter.c-					security_current_getsecid_subj(&sid);
kernel/auditfilter.c:					result = security_audit_rule_match(sid,
kernel/auditfilter.c-						   f->type, f->op, f->lsm_rule);
kernel/auditfilter.c-				}
kernel/auditfilter.c-				break;
```

---

# security hook: security_binder_set_context_mgr

drivers/android/binder.c:1

```shell
drivers/android/binder.c-		ret = -EBUSY;
drivers/android/binder.c-		goto out;
drivers/android/binder.c-	}
drivers/android/binder.c:	ret = security_binder_set_context_mgr(proc->cred);
drivers/android/binder.c-	if (ret < 0)
drivers/android/binder.c-		goto out;
drivers/android/binder.c-	if (uid_valid(context->binder_context_mgr_uid)) {
```

---

# security hook: security_binder_transaction

drivers/android/binder.c:1

```shell
drivers/android/binder.c-			return_error_line = __LINE__;
drivers/android/binder.c-			goto err_invalid_target_handle;
drivers/android/binder.c-		}
drivers/android/binder.c:		if (security_binder_transaction(proc->cred,
drivers/android/binder.c-						target_proc->cred) < 0) {
drivers/android/binder.c-			binder_txn_error("%d:%d transaction credentials failed\n",
drivers/android/binder.c-				thread->pid, proc->pid);
```

---

# security hook: security_binder_transfer_binder

drivers/android/binder.c:2

```shell
drivers/android/binder.c-		ret = -EINVAL;
drivers/android/binder.c-		goto done;
drivers/android/binder.c-	}
drivers/android/binder.c:	if (security_binder_transfer_binder(proc->cred, target_proc->cred)) {
drivers/android/binder.c-		ret = -EPERM;
drivers/android/binder.c-		goto done;
drivers/android/binder.c-	}
--
drivers/android/binder.c-				  proc->pid, thread->pid, fp->handle);
drivers/android/binder.c-		return -EINVAL;
drivers/android/binder.c-	}
drivers/android/binder.c:	if (security_binder_transfer_binder(proc->cred, target_proc->cred)) {
drivers/android/binder.c-		ret = -EPERM;
drivers/android/binder.c-		goto done;
drivers/android/binder.c-	}
```

---

# security hook: security_binder_transfer_file

drivers/android/binder.c:1

```shell
drivers/android/binder.c-		ret = -EBADF;
drivers/android/binder.c-		goto err_fget;
drivers/android/binder.c-	}
drivers/android/binder.c:	ret = security_binder_transfer_file(proc->cred, target_proc->cred, file);
drivers/android/binder.c-	if (ret < 0) {
drivers/android/binder.c-		ret = -EPERM;
drivers/android/binder.c-		goto err_security;
```

---

# security hook: security_bpf

kernel/bpf/inode.c:1
kernel/bpf/syscall.c:9

```shell
kernel/bpf/inode.c-
kernel/bpf/inode.c-	prog = inode->i_private;
kernel/bpf/inode.c-
kernel/bpf/inode.c:	ret = security_bpf_prog(prog);
kernel/bpf/inode.c-	if (ret < 0)
kernel/bpf/inode.c-		return ERR_PTR(ret);
kernel/bpf/inode.c-
--
kernel/bpf/syscall.c-	struct btf_record *rec = map->record;
kernel/bpf/syscall.c-	struct btf *btf = map->btf;
kernel/bpf/syscall.c-
kernel/bpf/syscall.c:	security_bpf_map_free(map);
kernel/bpf/syscall.c-	bpf_map_release_memcg(map);
kernel/bpf/syscall.c-	bpf_map_owner_free(map);
kernel/bpf/syscall.c-	/* implementation dependent freeing */
--
kernel/bpf/syscall.c-{
kernel/bpf/syscall.c-	int ret;
kernel/bpf/syscall.c-
kernel/bpf/syscall.c:	ret = security_bpf_map(map, OPEN_FMODE(flags));
kernel/bpf/syscall.c-	if (ret < 0)
kernel/bpf/syscall.c-		return ret;
kernel/bpf/syscall.c-
--
kernel/bpf/syscall.c-			attr->btf_vmlinux_value_type_id;
kernel/bpf/syscall.c-	}
kernel/bpf/syscall.c-
kernel/bpf/syscall.c:	err = security_bpf_map_alloc(map);
kernel/bpf/syscall.c-	if (err)
kernel/bpf/syscall.c-		goto free_map;
kernel/bpf/syscall.c-
--
kernel/bpf/syscall.c-	return err;
kernel/bpf/syscall.c-
kernel/bpf/syscall.c-free_map_sec:
kernel/bpf/syscall.c:	security_bpf_map_free(map);
kernel/bpf/syscall.c-free_map:
kernel/bpf/syscall.c-	btf_put(map->btf);
kernel/bpf/syscall.c-	map->ops->map_free(map);
--
kernel/bpf/syscall.c-	kvfree(aux->func_info);
kernel/bpf/syscall.c-	kfree(aux->func_info_aux);
kernel/bpf/syscall.c-	free_uid(aux->user);
kernel/bpf/syscall.c:	security_bpf_prog_free(aux);
kernel/bpf/syscall.c-	bpf_prog_free(aux->prog);
kernel/bpf/syscall.c-}
kernel/bpf/syscall.c-
--
kernel/bpf/syscall.c-{
kernel/bpf/syscall.c-	int ret;
kernel/bpf/syscall.c-
kernel/bpf/syscall.c:	ret = security_bpf_prog(prog);
kernel/bpf/syscall.c-	if (ret < 0)
kernel/bpf/syscall.c-		return ret;
kernel/bpf/syscall.c-
--
kernel/bpf/syscall.c-	prog->aux->sleepable = attr->prog_flags & BPF_F_SLEEPABLE;
kernel/bpf/syscall.c-	prog->aux->xdp_has_frags = attr->prog_flags & BPF_F_XDP_HAS_FRAGS;
kernel/bpf/syscall.c-
kernel/bpf/syscall.c:	err = security_bpf_prog_alloc(prog->aux);
kernel/bpf/syscall.c-	if (err)
kernel/bpf/syscall.c-		goto free_prog;
kernel/bpf/syscall.c-
--
kernel/bpf/syscall.c-	return err;
kernel/bpf/syscall.c-free_prog_sec:
kernel/bpf/syscall.c-	free_uid(prog->aux->user);
kernel/bpf/syscall.c:	security_bpf_prog_free(prog->aux);
kernel/bpf/syscall.c-free_prog:
kernel/bpf/syscall.c-	if (prog->aux->attach_btf)
kernel/bpf/syscall.c-		btf_put(prog->aux->attach_btf);
--
kernel/bpf/syscall.c-	if (copy_from_bpfptr(&attr, uattr, size) != 0)
kernel/bpf/syscall.c-		return -EFAULT;
kernel/bpf/syscall.c-
kernel/bpf/syscall.c:	err = security_bpf(cmd, &attr, size);
kernel/bpf/syscall.c-	if (err < 0)
kernel/bpf/syscall.c-		return err;
kernel/bpf/syscall.c-
```

---

# security hook: security_bpf_map

kernel/bpf/syscall.c:4

```shell
kernel/bpf/syscall.c-	struct btf_record *rec = map->record;
kernel/bpf/syscall.c-	struct btf *btf = map->btf;
kernel/bpf/syscall.c-
kernel/bpf/syscall.c:	security_bpf_map_free(map);
kernel/bpf/syscall.c-	bpf_map_release_memcg(map);
kernel/bpf/syscall.c-	bpf_map_owner_free(map);
kernel/bpf/syscall.c-	/* implementation dependent freeing */
--
kernel/bpf/syscall.c-{
kernel/bpf/syscall.c-	int ret;
kernel/bpf/syscall.c-
kernel/bpf/syscall.c:	ret = security_bpf_map(map, OPEN_FMODE(flags));
kernel/bpf/syscall.c-	if (ret < 0)
kernel/bpf/syscall.c-		return ret;
kernel/bpf/syscall.c-
--
kernel/bpf/syscall.c-			attr->btf_vmlinux_value_type_id;
kernel/bpf/syscall.c-	}
kernel/bpf/syscall.c-
kernel/bpf/syscall.c:	err = security_bpf_map_alloc(map);
kernel/bpf/syscall.c-	if (err)
kernel/bpf/syscall.c-		goto free_map;
kernel/bpf/syscall.c-
--
kernel/bpf/syscall.c-	return err;
kernel/bpf/syscall.c-
kernel/bpf/syscall.c-free_map_sec:
kernel/bpf/syscall.c:	security_bpf_map_free(map);
kernel/bpf/syscall.c-free_map:
kernel/bpf/syscall.c-	btf_put(map->btf);
kernel/bpf/syscall.c-	map->ops->map_free(map);
```

---

# security hook: security_bpf_map_alloc

kernel/bpf/syscall.c:1

```shell
kernel/bpf/syscall.c-			attr->btf_vmlinux_value_type_id;
kernel/bpf/syscall.c-	}
kernel/bpf/syscall.c-
kernel/bpf/syscall.c:	err = security_bpf_map_alloc(map);
kernel/bpf/syscall.c-	if (err)
kernel/bpf/syscall.c-		goto free_map;
kernel/bpf/syscall.c-
```

---

# security hook: security_bpf_map_free

kernel/bpf/syscall.c:2

```shell
kernel/bpf/syscall.c-	struct btf_record *rec = map->record;
kernel/bpf/syscall.c-	struct btf *btf = map->btf;
kernel/bpf/syscall.c-
kernel/bpf/syscall.c:	security_bpf_map_free(map);
kernel/bpf/syscall.c-	bpf_map_release_memcg(map);
kernel/bpf/syscall.c-	bpf_map_owner_free(map);
kernel/bpf/syscall.c-	/* implementation dependent freeing */
--
kernel/bpf/syscall.c-	return err;
kernel/bpf/syscall.c-
kernel/bpf/syscall.c-free_map_sec:
kernel/bpf/syscall.c:	security_bpf_map_free(map);
kernel/bpf/syscall.c-free_map:
kernel/bpf/syscall.c-	btf_put(map->btf);
kernel/bpf/syscall.c-	map->ops->map_free(map);
```

---

# security hook: security_bpf_prog

kernel/bpf/inode.c:1
kernel/bpf/syscall.c:4

```shell
kernel/bpf/inode.c-
kernel/bpf/inode.c-	prog = inode->i_private;
kernel/bpf/inode.c-
kernel/bpf/inode.c:	ret = security_bpf_prog(prog);
kernel/bpf/inode.c-	if (ret < 0)
kernel/bpf/inode.c-		return ERR_PTR(ret);
kernel/bpf/inode.c-
--
kernel/bpf/syscall.c-	kvfree(aux->func_info);
kernel/bpf/syscall.c-	kfree(aux->func_info_aux);
kernel/bpf/syscall.c-	free_uid(aux->user);
kernel/bpf/syscall.c:	security_bpf_prog_free(aux);
kernel/bpf/syscall.c-	bpf_prog_free(aux->prog);
kernel/bpf/syscall.c-}
kernel/bpf/syscall.c-
--
kernel/bpf/syscall.c-{
kernel/bpf/syscall.c-	int ret;
kernel/bpf/syscall.c-
kernel/bpf/syscall.c:	ret = security_bpf_prog(prog);
kernel/bpf/syscall.c-	if (ret < 0)
kernel/bpf/syscall.c-		return ret;
kernel/bpf/syscall.c-
--
kernel/bpf/syscall.c-	prog->aux->sleepable = attr->prog_flags & BPF_F_SLEEPABLE;
kernel/bpf/syscall.c-	prog->aux->xdp_has_frags = attr->prog_flags & BPF_F_XDP_HAS_FRAGS;
kernel/bpf/syscall.c-
kernel/bpf/syscall.c:	err = security_bpf_prog_alloc(prog->aux);
kernel/bpf/syscall.c-	if (err)
kernel/bpf/syscall.c-		goto free_prog;
kernel/bpf/syscall.c-
--
kernel/bpf/syscall.c-	return err;
kernel/bpf/syscall.c-free_prog_sec:
kernel/bpf/syscall.c-	free_uid(prog->aux->user);
kernel/bpf/syscall.c:	security_bpf_prog_free(prog->aux);
kernel/bpf/syscall.c-free_prog:
kernel/bpf/syscall.c-	if (prog->aux->attach_btf)
kernel/bpf/syscall.c-		btf_put(prog->aux->attach_btf);
```

---

# security hook: security_bpf_prog_alloc

kernel/bpf/syscall.c:1

```shell
kernel/bpf/syscall.c-	prog->aux->sleepable = attr->prog_flags & BPF_F_SLEEPABLE;
kernel/bpf/syscall.c-	prog->aux->xdp_has_frags = attr->prog_flags & BPF_F_XDP_HAS_FRAGS;
kernel/bpf/syscall.c-
kernel/bpf/syscall.c:	err = security_bpf_prog_alloc(prog->aux);
kernel/bpf/syscall.c-	if (err)
kernel/bpf/syscall.c-		goto free_prog;
kernel/bpf/syscall.c-
```

---

# security hook: security_bpf_prog_free

kernel/bpf/syscall.c:2

```shell
kernel/bpf/syscall.c-	kvfree(aux->func_info);
kernel/bpf/syscall.c-	kfree(aux->func_info_aux);
kernel/bpf/syscall.c-	free_uid(aux->user);
kernel/bpf/syscall.c:	security_bpf_prog_free(aux);
kernel/bpf/syscall.c-	bpf_prog_free(aux->prog);
kernel/bpf/syscall.c-}
kernel/bpf/syscall.c-
--
kernel/bpf/syscall.c-	return err;
kernel/bpf/syscall.c-free_prog_sec:
kernel/bpf/syscall.c-	free_uid(prog->aux->user);
kernel/bpf/syscall.c:	security_bpf_prog_free(prog->aux);
kernel/bpf/syscall.c-free_prog:
kernel/bpf/syscall.c-	if (prog->aux->attach_btf)
kernel/bpf/syscall.c-		btf_put(prog->aux->attach_btf);
```

---

# security hook: security_bprm_check

fs/exec.c:2

```shell
fs/exec.c-	 * Indeed, the kernel should not try to parse the content of the file
fs/exec.c-	 * with exec_binprm() nor change the calling thread, which means that
fs/exec.c-	 * the following security functions will be not called:
fs/exec.c:	 * - security_bprm_check()
fs/exec.c-	 * - security_bprm_creds_from_file()
fs/exec.c-	 * - security_bprm_committing_creds()
fs/exec.c-	 * - security_bprm_committed_creds()
--
fs/exec.c-	if (retval < 0)
fs/exec.c-		return retval;
fs/exec.c-
fs/exec.c:	retval = security_bprm_check(bprm);
fs/exec.c-	if (retval)
fs/exec.c-		return retval;
fs/exec.c-
```

---

# security hook: security_bprm_committed_creds

fs/exec.c:2

```shell
fs/exec.c-	 * ptrace_attach() from altering our determination of the task's
fs/exec.c-	 * credentials; any time after this it may be unlocked.
fs/exec.c-	 */
fs/exec.c:	security_bprm_committed_creds(bprm);
fs/exec.c-
fs/exec.c-	/* Pass the opened binary to the interpreter. */
fs/exec.c-	if (bprm->have_execfd) {
--
fs/exec.c-	 * - security_bprm_check()
fs/exec.c-	 * - security_bprm_creds_from_file()
fs/exec.c-	 * - security_bprm_committing_creds()
fs/exec.c:	 * - security_bprm_committed_creds()
fs/exec.c-	 */
fs/exec.c-	bprm->is_check = !!(flags & AT_CHECK);
fs/exec.c-
```

---

# security hook: security_bprm_committing_creds

fs/exec.c:2

```shell
fs/exec.c-	/*
fs/exec.c-	 * install the new credentials for this executable
fs/exec.c-	 */
fs/exec.c:	security_bprm_committing_creds(bprm);
fs/exec.c-
fs/exec.c-	commit_creds(bprm->cred);
fs/exec.c-	bprm->cred = NULL;
--
fs/exec.c-	 * the following security functions will be not called:
fs/exec.c-	 * - security_bprm_check()
fs/exec.c-	 * - security_bprm_creds_from_file()
fs/exec.c:	 * - security_bprm_committing_creds()
fs/exec.c-	 * - security_bprm_committed_creds()
fs/exec.c-	 */
fs/exec.c-	bprm->is_check = !!(flags & AT_CHECK);
```

---

# security hook: security_bprm_creds_for_exec

fs/exec.c:2

```shell
fs/exec.c-	/*
fs/exec.c-	 * At this point, security_file_open() has already been called (with
fs/exec.c-	 * __FMODE_EXEC) and access control checks for AT_CHECK will stop just
fs/exec.c:	 * after the security_bprm_creds_for_exec() call in bprm_execve().
fs/exec.c-	 * Indeed, the kernel should not try to parse the content of the file
fs/exec.c-	 * with exec_binprm() nor change the calling thread, which means that
fs/exec.c-	 * the following security functions will be not called:
--
fs/exec.c-	sched_exec();
fs/exec.c-
fs/exec.c-	/* Set the unchanging part of bprm->cred */
fs/exec.c:	retval = security_bprm_creds_for_exec(bprm);
fs/exec.c-	if (retval || bprm->is_check)
fs/exec.c-		goto out;
fs/exec.c-
```

---

# security hook: security_bprm_creds_from_file

fs/exec.c:2

```shell
fs/exec.c-	 * with exec_binprm() nor change the calling thread, which means that
fs/exec.c-	 * the following security functions will be not called:
fs/exec.c-	 * - security_bprm_check()
fs/exec.c:	 * - security_bprm_creds_from_file()
fs/exec.c-	 * - security_bprm_committing_creds()
fs/exec.c-	 * - security_bprm_committed_creds()
fs/exec.c-	 */
--
fs/exec.c-	struct file *file = bprm->execfd_creds ? bprm->executable : bprm->file;
fs/exec.c-
fs/exec.c-	bprm_fill_uid(bprm, file);
fs/exec.c:	return security_bprm_creds_from_file(bprm, file);
fs/exec.c-}
fs/exec.c-
fs/exec.c-/*
```

---

# security hook: security_capable

kernel/capability.c:5
kernel/ksyms_common.c:1

```shell
kernel/capability.c-	int ret;
kernel/capability.c-
kernel/capability.c-	rcu_read_lock();
kernel/capability.c:	ret = security_capable(__task_cred(t), ns, cap, CAP_OPT_NONE);
kernel/capability.c-	rcu_read_unlock();
kernel/capability.c-
kernel/capability.c-	return (ret == 0);
--
kernel/capability.c-	int ret;
kernel/capability.c-
kernel/capability.c-	rcu_read_lock();
kernel/capability.c:	ret = security_capable(__task_cred(t), ns, cap, CAP_OPT_NOAUDIT);
kernel/capability.c-	rcu_read_unlock();
kernel/capability.c-
kernel/capability.c-	return (ret == 0);
--
kernel/capability.c-		BUG();
kernel/capability.c-	}
kernel/capability.c-
kernel/capability.c:	capable = security_capable(current_cred(), ns, cap, opts);
kernel/capability.c-	if (capable == 0) {
kernel/capability.c-		current->flags |= PF_SUPERPRIV;
kernel/capability.c-		return true;
--
kernel/capability.c-	if (WARN_ON_ONCE(!cap_valid(cap)))
kernel/capability.c-		return false;
kernel/capability.c-
kernel/capability.c:	if (security_capable(file->f_cred, ns, cap, CAP_OPT_NONE) == 0)
kernel/capability.c-		return true;
kernel/capability.c-
kernel/capability.c-	return false;
--
kernel/capability.c-	rcu_read_lock();
kernel/capability.c-	cred = rcu_dereference(tsk->ptracer_cred);
kernel/capability.c-	if (cred)
kernel/capability.c:		ret = security_capable(cred, ns, CAP_SYS_PTRACE,
kernel/capability.c-				       CAP_OPT_NOAUDIT);
kernel/capability.c-	rcu_read_unlock();
kernel/capability.c-	return (ret == 0);
--
kernel/ksyms_common.c-			return true;
kernel/ksyms_common.c-		fallthrough;
kernel/ksyms_common.c-	case 1:
kernel/ksyms_common.c:		if (security_capable(cred, &init_user_ns, CAP_SYSLOG,
kernel/ksyms_common.c-				     CAP_OPT_NOAUDIT) == 0)
kernel/ksyms_common.c-			return true;
kernel/ksyms_common.c-		fallthrough;
```

---

# security hook: security_capget

kernel/capability.c:2

```shell
kernel/capability.c-		if (!target)
kernel/capability.c-			ret = -ESRCH;
kernel/capability.c-		else
kernel/capability.c:			ret = security_capget(target, pEp, pIp, pPp);
kernel/capability.c-
kernel/capability.c-		rcu_read_unlock();
kernel/capability.c-	} else
kernel/capability.c:		ret = security_capget(current, pEp, pIp, pPp);
kernel/capability.c-
kernel/capability.c-	return ret;
kernel/capability.c-}
```

---

# security hook: security_capset

kernel/capability.c:1

```shell
kernel/capability.c-	if (!new)
kernel/capability.c-		return -ENOMEM;
kernel/capability.c-
kernel/capability.c:	ret = security_capset(new, current_cred(),
kernel/capability.c-			      &effective, &inheritable, &permitted);
kernel/capability.c-	if (ret < 0)
kernel/capability.c-		goto error;
```

---

# security hook: security_create_user_ns

kernel/user_namespace.c:1

```shell
kernel/user_namespace.c-	    !kgid_has_mapping(parent_ns, group))
kernel/user_namespace.c-		goto fail_dec;
kernel/user_namespace.c-
kernel/user_namespace.c:	ret = security_create_user_ns(new);
kernel/user_namespace.c-	if (ret < 0)
kernel/user_namespace.c-		goto fail_dec;
kernel/user_namespace.c-
```

---

# security hook: security_cred_alloc_blank

kernel/cred.c:1

```shell
kernel/cred.c-		return NULL;
kernel/cred.c-
kernel/cred.c-	atomic_long_set(&new->usage, 1);
kernel/cred.c:	if (security_cred_alloc_blank(new, GFP_KERNEL_ACCOUNT) < 0)
kernel/cred.c-		goto error;
kernel/cred.c-
kernel/cred.c-	return new;
```

---

# security hook: security_cred_free

kernel/cred.c:1

```shell
kernel/cred.c-		panic("CRED: put_cred_rcu() sees %p with usage %ld\n",
kernel/cred.c-		      cred, atomic_long_read(&cred->usage));
kernel/cred.c-
kernel/cred.c:	security_cred_free(cred);
kernel/cred.c-	key_put(cred->session_keyring);
kernel/cred.c-	key_put(cred->process_keyring);
kernel/cred.c-	key_put(cred->thread_keyring);
```

---

# security hook: security_cred_getsecid

drivers/android/binder.c:1

```shell
drivers/android/binder.c-		u32 secid;
drivers/android/binder.c-		size_t added_size;
drivers/android/binder.c-
drivers/android/binder.c:		security_cred_getsecid(proc->cred, &secid);
drivers/android/binder.c-		ret = security_secid_to_secctx(secid, &secctx, &secctx_sz);
drivers/android/binder.c-		if (ret) {
drivers/android/binder.c-			binder_txn_error("%d:%d failed to get security context\n",
```

---

# security hook: security_current_getsecid_subj

net/netlabel/netlabel_user.h:1
net/netlabel/netlabel_unlabeled.c:1
kernel/auditsc.c:2
kernel/auditfilter.c:1
kernel/audit.c:2

```shell
net/netlabel/netlabel_user.h- */
net/netlabel/netlabel_user.h-static inline void netlbl_netlink_auditinfo(struct netlbl_audit *audit_info)
net/netlabel/netlabel_user.h-{
net/netlabel/netlabel_user.h:	security_current_getsecid_subj(&audit_info->secid);
net/netlabel/netlabel_user.h-	audit_info->loginuid = audit_get_loginuid(current);
net/netlabel/netlabel_user.h-	audit_info->sessionid = audit_get_sessionid(current);
net/netlabel/netlabel_user.h-}
--
net/netlabel/netlabel_unlabeled.c-	/* Only the kernel is allowed to call this function and the only time
net/netlabel/netlabel_unlabeled.c-	 * it is called is at bootup before the audit subsystem is reporting
net/netlabel/netlabel_unlabeled.c-	 * messages so don't worry to much about these values. */
net/netlabel/netlabel_unlabeled.c:	security_current_getsecid_subj(&audit_info.secid);
net/netlabel/netlabel_unlabeled.c-	audit_info.loginuid = GLOBAL_ROOT_UID;
net/netlabel/netlabel_unlabeled.c-	audit_info.sessionid = 0;
net/netlabel/netlabel_unlabeled.c-
--
kernel/audit.c-	int error;
kernel/audit.c-	u32 sid;
kernel/audit.c-
kernel/audit.c:	security_current_getsecid_subj(&sid);
kernel/audit.c-	if (!sid)
kernel/audit.c-		return 0;
kernel/audit.c-
--
kernel/audit.c-			audit_sig_uid = auid;
kernel/audit.c-		else
kernel/audit.c-			audit_sig_uid = uid;
kernel/audit.c:		security_current_getsecid_subj(&audit_sig_sid);
kernel/audit.c-	}
kernel/audit.c-
kernel/audit.c-	return audit_signal_info_syscall(t);
--
kernel/auditsc.c-					 * fork()/copy_process() in which case
kernel/auditsc.c-					 * the new @tsk creds are still a dup
kernel/auditsc.c-					 * of @current's creds so we can still
kernel/auditsc.c:					 * use security_current_getsecid_subj()
kernel/auditsc.c-					 * here even though it always refs
kernel/auditsc.c-					 * @current's creds
kernel/auditsc.c-					 */
kernel/auditsc.c:					security_current_getsecid_subj(&sid);
kernel/auditsc.c-					need_sid = 0;
kernel/auditsc.c-				}
kernel/auditsc.c-				result = security_audit_rule_match(sid, f->type,
--
kernel/auditfilter.c-			case AUDIT_SUBJ_SEN:
kernel/auditfilter.c-			case AUDIT_SUBJ_CLR:
kernel/auditfilter.c-				if (f->lsm_rule) {
kernel/auditfilter.c:					security_current_getsecid_subj(&sid);
kernel/auditfilter.c-					result = security_audit_rule_match(sid,
kernel/auditfilter.c-						   f->type, f->op, f->lsm_rule);
kernel/auditfilter.c-				}
```

---

# security hook: security_dentry_create_files_as

fs/overlayfs/dir.c:1

```shell
fs/overlayfs/dir.c-		 */
fs/overlayfs/dir.c-		override_cred->fsuid = inode->i_uid;
fs/overlayfs/dir.c-		override_cred->fsgid = inode->i_gid;
fs/overlayfs/dir.c:		err = security_dentry_create_files_as(dentry,
fs/overlayfs/dir.c-				attr->mode, &dentry->d_name, old_cred,
fs/overlayfs/dir.c-				override_cred);
fs/overlayfs/dir.c-		if (err) {
```

---

# security hook: security_dentry_init_security

fs/fuse/dir.c:1
fs/ceph/xattr.c:2
fs/nfs/nfs4proc.c:1

```shell
fs/fuse/dir.c-	const char *name;
fs/fuse/dir.c-	size_t namelen;
fs/fuse/dir.c-
fs/fuse/dir.c:	err = security_dentry_init_security(entry, mode, &entry->d_name,
fs/fuse/dir.c-					    &name, &ctx, &ctxlen);
fs/fuse/dir.c-	if (err) {
fs/fuse/dir.c-		if (err != -EOPNOTSUPP)
--
fs/ceph/xattr.c-	size_t name_len;
fs/ceph/xattr.c-	int err;
fs/ceph/xattr.c-
fs/ceph/xattr.c:	err = security_dentry_init_security(dentry, mode, &dentry->d_name,
fs/ceph/xattr.c-					    &name, &as_ctx->sec_ctx,
fs/ceph/xattr.c-					    &as_ctx->sec_ctxlen);
fs/ceph/xattr.c-	if (err < 0) {
--
fs/ceph/xattr.c-	}
fs/ceph/xattr.c-
fs/ceph/xattr.c-	/*
fs/ceph/xattr.c:	 * FIXME: Make security_dentry_init_security() generic. Currently
fs/ceph/xattr.c-	 * It only supports single security module and only selinux has
fs/ceph/xattr.c-	 * dentry_init_security hook.
fs/ceph/xattr.c-	 */
--
fs/nfs/nfs4proc.c-	label->len = 0;
fs/nfs/nfs4proc.c-	label->label = NULL;
fs/nfs/nfs4proc.c-
fs/nfs/nfs4proc.c:	err = security_dentry_init_security(dentry, sattr->ia_mode,
fs/nfs/nfs4proc.c-				&dentry->d_name, NULL,
fs/nfs/nfs4proc.c-				(void **)&label->label, &label->len);
fs/nfs/nfs4proc.c-	if (err == 0)
```

---

# security hook: security_d_instantiate

Documentation/filesystems/porting.rst:2
fs/dcache.c:5
fs/nfs/getroot.c:1

```shell
Documentation/filesystems/porting.rst-
Documentation/filesystems/porting.rst-->getxattr() and xattr_handler.get() get dentry and inode passed separately.
Documentation/filesystems/porting.rst-dentry might be yet to be attached to inode, so do _not_ use its ->d_inode
Documentation/filesystems/porting.rst:in the instances.  Rationale: !@#!@# security_d_instantiate() needs to be
Documentation/filesystems/porting.rst-called before we attach dentry to inode.
Documentation/filesystems/porting.rst-
Documentation/filesystems/porting.rst----
--
Documentation/filesystems/porting.rst-The xattr_handler.set() gets passed the user namespace of the mount the inode
Documentation/filesystems/porting.rst-is seen from so filesystems can idmap the i_uid and i_gid accordingly.
Documentation/filesystems/porting.rst-dentry might be yet to be attached to inode, so do _not_ use its ->d_inode
Documentation/filesystems/porting.rst:in the instances.  Rationale: !@#!@# security_d_instantiate() needs to be
Documentation/filesystems/porting.rst-called before we attach dentry to inode and !@#!@##!@$!$#!@#$!@$!@$ smack
Documentation/filesystems/porting.rst-->d_instantiate() uses not just ->getxattr() but ->setxattr() as well.
Documentation/filesystems/porting.rst-
--
fs/dcache.c-{
fs/dcache.c-	BUG_ON(!hlist_unhashed(&entry->d_u.d_alias));
fs/dcache.c-	if (inode) {
fs/dcache.c:		security_d_instantiate(entry, inode);
fs/dcache.c-		spin_lock(&inode->i_lock);
fs/dcache.c-		__d_instantiate(entry, inode);
fs/dcache.c-		spin_unlock(&inode->i_lock);
--
fs/dcache.c-	BUG_ON(!hlist_unhashed(&entry->d_u.d_alias));
fs/dcache.c-	BUG_ON(!inode);
fs/dcache.c-	lockdep_annotate_inode_mutex_key(inode);
fs/dcache.c:	security_d_instantiate(entry, inode);
fs/dcache.c-	spin_lock(&inode->i_lock);
fs/dcache.c-	__d_instantiate(entry, inode);
fs/dcache.c-	WARN_ON(!(inode->i_state & I_NEW));
--
fs/dcache.c-	struct dentry *res;
fs/dcache.c-	unsigned add_flags;
fs/dcache.c-
fs/dcache.c:	security_d_instantiate(dentry, inode);
fs/dcache.c-	spin_lock(&inode->i_lock);
fs/dcache.c-	res = __d_find_any_alias(inode);
fs/dcache.c-	if (res) {
--
fs/dcache.c-void d_add(struct dentry *entry, struct inode *inode)
fs/dcache.c-{
fs/dcache.c-	if (inode) {
fs/dcache.c:		security_d_instantiate(entry, inode);
fs/dcache.c-		spin_lock(&inode->i_lock);
fs/dcache.c-	}
fs/dcache.c-	__d_add(entry, inode);
--
fs/dcache.c-	if (!inode)
fs/dcache.c-		goto out;
fs/dcache.c-
fs/dcache.c:	security_d_instantiate(dentry, inode);
fs/dcache.c-	spin_lock(&inode->i_lock);
fs/dcache.c-	if (S_ISDIR(inode->i_mode)) {
fs/dcache.c-		struct dentry *new = __d_find_any_alias(inode);
--
fs/nfs/getroot.c-		goto out_fattr;
fs/nfs/getroot.c-	}
fs/nfs/getroot.c-
fs/nfs/getroot.c:	security_d_instantiate(root, inode);
fs/nfs/getroot.c-	spin_lock(&root->d_lock);
fs/nfs/getroot.c-	if (IS_ROOT(root) && !root->d_fsdata &&
fs/nfs/getroot.c-	    !(root->d_flags & DCACHE_NFSFS_RENAMED)) {
```

---

# security hook: security_file_alloc

Documentation/trace/histogram.rst:1
fs/file_table.c:1

```shell
Documentation/trace/histogram.rst-    { common_stacktrace:
Documentation/trace/histogram.rst-         kmem_cache_alloc_trace+0xeb/0x150
Documentation/trace/histogram.rst-         apparmor_file_alloc_security+0x27/0x40
Documentation/trace/histogram.rst:         security_file_alloc+0x16/0x20
Documentation/trace/histogram.rst-         get_empty_filp+0x93/0x1c0
Documentation/trace/histogram.rst-         path_openat+0x31/0x5f0
Documentation/trace/histogram.rst-         do_filp_open+0x3a/0x90
--
fs/file_table.c-	int error;
fs/file_table.c-
fs/file_table.c-	f->f_cred = get_cred(cred);
fs/file_table.c:	error = security_file_alloc(f);
fs/file_table.c-	if (unlikely(error)) {
fs/file_table.c-		put_cred(f->f_cred);
fs/file_table.c-		return error;
```

---

# security hook: security_file_fcntl

fs/fcntl.c:3
arch/arm/kernel/sys_oabi-compat.c:2

```shell
arch/arm/kernel/sys_oabi-compat.c-	switch (cmd) {
arch/arm/kernel/sys_oabi-compat.c-	case F_GETLK64:
arch/arm/kernel/sys_oabi-compat.c-	case F_OFD_GETLK:
arch/arm/kernel/sys_oabi-compat.c:		err = security_file_fcntl(f.file, cmd, arg);
arch/arm/kernel/sys_oabi-compat.c-		if (err)
arch/arm/kernel/sys_oabi-compat.c-			break;
arch/arm/kernel/sys_oabi-compat.c-		err = get_oabi_flock(&flock, argp);
--
arch/arm/kernel/sys_oabi-compat.c-	case F_SETLKW64:
arch/arm/kernel/sys_oabi-compat.c-	case F_OFD_SETLK:
arch/arm/kernel/sys_oabi-compat.c-	case F_OFD_SETLKW:
arch/arm/kernel/sys_oabi-compat.c:		err = security_file_fcntl(f.file, cmd, arg);
arch/arm/kernel/sys_oabi-compat.c-		if (err)
arch/arm/kernel/sys_oabi-compat.c-			break;
arch/arm/kernel/sys_oabi-compat.c-		err = get_oabi_flock(&flock, argp);
--
fs/fcntl.c-			goto out1;
fs/fcntl.c-	}
fs/fcntl.c-
fs/fcntl.c:	err = security_file_fcntl(f.file, cmd, arg);
fs/fcntl.c-	if (!err)
fs/fcntl.c-		err = do_fcntl(fd, cmd, arg, f.file);
fs/fcntl.c-
--
fs/fcntl.c-			goto out1;
fs/fcntl.c-	}
fs/fcntl.c-
fs/fcntl.c:	err = security_file_fcntl(f.file, cmd, arg);
fs/fcntl.c-	if (err)
fs/fcntl.c-		goto out1;
fs/fcntl.c-	
--
fs/fcntl.c-			goto out_put;
fs/fcntl.c-	}
fs/fcntl.c-
fs/fcntl.c:	err = security_file_fcntl(f.file, cmd, arg);
fs/fcntl.c-	if (err)
fs/fcntl.c-		goto out_put;
fs/fcntl.c-
```

---

# security hook: security_file_free

fs/file_table.c:1

```shell
fs/file_table.c-
fs/file_table.c-static inline void file_free(struct file *f)
fs/file_table.c-{
fs/file_table.c:	security_file_free(f);
fs/file_table.c-	if (unlikely(f->f_mode & FMODE_BACKING))
fs/file_table.c-		path_put(backing_file_user_path(f));
fs/file_table.c-	if (likely(!(f->f_mode & FMODE_NOACCOUNT)))
```

---

# security hook: security_file_ioctl

fs/overlayfs/inode.c:2
fs/ioctl.c:2

```shell
fs/overlayfs/inode.c-}
fs/overlayfs/inode.c-
fs/overlayfs/inode.c-/*
fs/overlayfs/inode.c: * Work around the fact that security_file_ioctl() takes a file argument.
fs/overlayfs/inode.c- * Introducing security_inode_fileattr_get/set() hooks would solve this issue
fs/overlayfs/inode.c- * properly.
fs/overlayfs/inode.c- */
--
fs/overlayfs/inode.c-	else
fs/overlayfs/inode.c-		cmd = fa->fsx_valid ? FS_IOC_FSGETXATTR : FS_IOC_GETFLAGS;
fs/overlayfs/inode.c-
fs/overlayfs/inode.c:	err = security_file_ioctl(file, cmd, 0);
fs/overlayfs/inode.c-	fput(file);
fs/overlayfs/inode.c-
fs/overlayfs/inode.c-	return err;
--
fs/ioctl.c-	if (!f.file)
fs/ioctl.c-		return -EBADF;
fs/ioctl.c-
fs/ioctl.c:	error = security_file_ioctl(f.file, cmd, arg);
fs/ioctl.c-	if (error)
fs/ioctl.c-		goto out;
fs/ioctl.c-
--
fs/ioctl.c-	if (!f.file)
fs/ioctl.c-		return -EBADF;
fs/ioctl.c-
fs/ioctl.c:	error = security_file_ioctl_compat(f.file, cmd, arg);
fs/ioctl.c-	if (error)
fs/ioctl.c-		goto out;
fs/ioctl.c-
```

---

# security hook: security_file_ioctl_compat

fs/ioctl.c:1

```shell
fs/ioctl.c-	if (!f.file)
fs/ioctl.c-		return -EBADF;
fs/ioctl.c-
fs/ioctl.c:	error = security_file_ioctl_compat(f.file, cmd, arg);
fs/ioctl.c-	if (error)
fs/ioctl.c-		goto out;
fs/ioctl.c-
```

---

# security hook: security_file_lock

fs/locks.c:3

```shell
fs/locks.c-		return -EACCES;
fs/locks.c-	if (!S_ISREG(inode->i_mode))
fs/locks.c-		return -EINVAL;
fs/locks.c:	error = security_file_lock(filp, arg);
fs/locks.c-	if (error)
fs/locks.c-		return error;
fs/locks.c-
--
fs/locks.c-
fs/locks.c-	flock_make_lock(f.file, &fl, type);
fs/locks.c-
fs/locks.c:	error = security_file_lock(f.file, fl.fl_type);
fs/locks.c-	if (error)
fs/locks.c-		goto out_putf;
fs/locks.c-
--
fs/locks.c-{
fs/locks.c-	int error;
fs/locks.c-
fs/locks.c:	error = security_file_lock(filp, fl->fl_type);
fs/locks.c-	if (error)
fs/locks.c-		return error;
fs/locks.c-
```

---

# security hook: security_file_mprotect

mm/mprotect.c:1

```shell
mm/mprotect.c-			break;
mm/mprotect.c-		}
mm/mprotect.c-
mm/mprotect.c:		error = security_file_mprotect(vma, reqprot, prot);
mm/mprotect.c-		if (error)
mm/mprotect.c-			break;
mm/mprotect.c-
```

---

# security hook: security_file_open

kernel/trace/bpf_trace.c:1
fs/open.c:1
fs/exec.c:1

```shell
kernel/trace/bpf_trace.c-#ifdef CONFIG_SECURITY
kernel/trace/bpf_trace.c-BTF_ID(func, security_file_permission)
kernel/trace/bpf_trace.c-BTF_ID(func, security_inode_getattr)
kernel/trace/bpf_trace.c:BTF_ID(func, security_file_open)
kernel/trace/bpf_trace.c-#endif
kernel/trace/bpf_trace.c-#ifdef CONFIG_SECURITY_PATH
kernel/trace/bpf_trace.c-BTF_ID(func, security_path_truncate)
--
fs/exec.c-	bprm->interp = bprm->filename;
fs/exec.c-
fs/exec.c-	/*
fs/exec.c:	 * At this point, security_file_open() has already been called (with
fs/exec.c-	 * __FMODE_EXEC) and access control checks for AT_CHECK will stop just
fs/exec.c-	 * after the security_bprm_creds_for_exec() call in bprm_execve().
fs/exec.c-	 * Indeed, the kernel should not try to parse the content of the file
--
fs/open.c-		goto cleanup_all;
fs/open.c-	}
fs/open.c-
fs/open.c:	error = security_file_open(f);
fs/open.c-	if (error)
fs/open.c-		goto cleanup_all;
fs/open.c-
```

---

# security hook: security_file_permission

kernel/trace/bpf_trace.c:1
tools/perf/util/annotate.c:1
fs/open.c:1
fs/read_write.c:1
fs/readdir.c:1
fs/remap_range.c:1

```shell
kernel/trace/bpf_trace.c-
kernel/trace/bpf_trace.c-BTF_SET_START(btf_allowlist_d_path)
kernel/trace/bpf_trace.c-#ifdef CONFIG_SECURITY
kernel/trace/bpf_trace.c:BTF_ID(func, security_file_permission)
kernel/trace/bpf_trace.c-BTF_ID(func, security_inode_getattr)
kernel/trace/bpf_trace.c-BTF_ID(func, security_file_open)
kernel/trace/bpf_trace.c-#endif
--
tools/perf/util/annotate.c-	 */
tools/perf/util/annotate.c-	/*
tools/perf/util/annotate.c-	 * skip over possible up to 2 operands to get to address, e.g.:
tools/perf/util/annotate.c:	 * tbnz	 w0, #26, ffff0000083cd190 <security_file_permission+0xd0>
tools/perf/util/annotate.c-	 */
tools/perf/util/annotate.c-	if (c++ != NULL) {
tools/perf/util/annotate.c-		ops->target.addr = strtoull(c, NULL, 16);
--
fs/open.c-	 * Revalidate the write permissions, in case security policy has
fs/open.c-	 * changed since the files were opened.
fs/open.c-	 */
fs/open.c:	ret = security_file_permission(file, MAY_WRITE);
fs/open.c-	if (ret)
fs/open.c-		return ret;
fs/open.c-
--
fs/read_write.c-		}
fs/read_write.c-	}
fs/read_write.c-
fs/read_write.c:	return security_file_permission(file,
fs/read_write.c-				read_write == READ ? MAY_READ : MAY_WRITE);
fs/read_write.c-}
fs/read_write.c-EXPORT_SYMBOL(rw_verify_area);
--
fs/readdir.c-	if (!file->f_op->iterate_shared)
fs/readdir.c-		goto out;
fs/readdir.c-
fs/readdir.c:	res = security_file_permission(file, MAY_READ);
fs/readdir.c-	if (res)
fs/readdir.c-		goto out;
fs/readdir.c-
--
fs/remap_range.c-	if (unlikely(check_add_overflow(pos, len, &tmp)))
fs/remap_range.c-		return -EINVAL;
fs/remap_range.c-
fs/remap_range.c:	return security_file_permission(file, write ? MAY_WRITE : MAY_READ);
fs/remap_range.c-}
fs/remap_range.c-
fs/remap_range.c-/*
```

---

# security hook: security_file_receive

fs/file.c:2

```shell
fs/file.c-	int new_fd;
fs/file.c-	int error;
fs/file.c-
fs/file.c:	error = security_file_receive(file);
fs/file.c-	if (error)
fs/file.c-		return error;
fs/file.c-
--
fs/file.c-{
fs/file.c-	int error;
fs/file.c-
fs/file.c:	error = security_file_receive(file);
fs/file.c-	if (error)
fs/file.c-		return error;
fs/file.c-	error = replace_fd(new_fd, file, o_flags);
```

---

# security hook: security_file_send_sigiotask

fs/fcntl.c:1

```shell
fs/fcntl.c-	ret = ((uid_eq(fown->euid, GLOBAL_ROOT_UID) ||
fs/fcntl.c-		uid_eq(fown->euid, cred->suid) || uid_eq(fown->euid, cred->uid) ||
fs/fcntl.c-		uid_eq(fown->uid,  cred->suid) || uid_eq(fown->uid,  cred->uid)) &&
fs/fcntl.c:	       !security_file_send_sigiotask(p, fown, sig));
fs/fcntl.c-	rcu_read_unlock();
fs/fcntl.c-	return ret;
fs/fcntl.c-}
```

---

# security hook: security_file_set_fowner

fs/fcntl.c:1

```shell
fs/fcntl.c-
fs/fcntl.c-		if (pid) {
fs/fcntl.c-			const struct cred *cred = current_cred();
fs/fcntl.c:			security_file_set_fowner(filp);
fs/fcntl.c-			filp->f_owner.uid = cred->uid;
fs/fcntl.c-			filp->f_owner.euid = cred->euid;
fs/fcntl.c-		}
```

---

# security hook: security_file_truncate

fs/open.c:1
fs/namei.c:1

```shell
fs/open.c-	if (IS_APPEND(file_inode(f.file)))
fs/open.c-		goto out_putf;
fs/open.c-	sb_start_write(inode->i_sb);
fs/open.c:	error = security_file_truncate(f.file);
fs/open.c-	if (!error)
fs/open.c-		error = do_truncate(file_mnt_idmap(f.file), dentry, length,
fs/open.c-				    ATTR_MTIME | ATTR_CTIME, f.file);
--
fs/namei.c-	if (error)
fs/namei.c-		return error;
fs/namei.c-
fs/namei.c:	error = security_file_truncate(filp);
fs/namei.c-	if (!error) {
fs/namei.c-		error = do_truncate(idmap, path->dentry, 0,
fs/namei.c-				    ATTR_MTIME|ATTR_CTIME|ATTR_OPEN,
```

---

# security hook: security_free_mnt_opts

fs/btrfs/super.c:3
fs/fs_context.c:2

```shell
fs/fs_context.c-	if (fc->need_free && fc->ops && fc->ops->free)
fs/fs_context.c-		fc->ops->free(fc);
fs/fs_context.c-
fs/fs_context.c:	security_free_mnt_opts(&fc->security);
fs/fs_context.c-	put_net(fc->net_ns);
fs/fs_context.c-	put_user_ns(fc->user_ns);
fs/fs_context.c-	put_cred(fc->cred);
--
fs/fs_context.c-	fc->fs_private = NULL;
fs/fs_context.c-	fc->s_fs_info = NULL;
fs/fs_context.c-	fc->sb_flags = 0;
fs/fs_context.c:	security_free_mnt_opts(&fc->security);
fs/fs_context.c-	kfree(fc->source);
fs/fs_context.c-	fc->source = NULL;
fs/fs_context.c-	fc->exclusive = false;
--
fs/btrfs/super.c-	}
fs/btrfs/super.c-	if (!error)
fs/btrfs/super.c-		error = security_sb_set_mnt_opts(s, new_sec_opts, 0, NULL);
fs/btrfs/super.c:	security_free_mnt_opts(&new_sec_opts);
fs/btrfs/super.c-	if (error) {
fs/btrfs/super.c-		deactivate_locked_super(s);
fs/btrfs/super.c-		return ERR_PTR(error);
--
fs/btrfs/super.c-error_fs_info:
fs/btrfs/super.c-	btrfs_free_fs_info(fs_info);
fs/btrfs/super.c-error_sec_opts:
fs/btrfs/super.c:	security_free_mnt_opts(&new_sec_opts);
fs/btrfs/super.c-	return ERR_PTR(error);
fs/btrfs/super.c-}
fs/btrfs/super.c-
--
fs/btrfs/super.c-		ret = security_sb_eat_lsm_opts(data, &new_sec_opts);
fs/btrfs/super.c-		if (!ret)
fs/btrfs/super.c-			ret = security_sb_remount(sb, new_sec_opts);
fs/btrfs/super.c:		security_free_mnt_opts(&new_sec_opts);
fs/btrfs/super.c-		if (ret)
fs/btrfs/super.c-			goto restore;
fs/btrfs/super.c-	}
```

---

# security hook: security_fs_context_dup

Documentation/filesystems/mount_api.rst:1
fs/fs_context.c:1

```shell
Documentation/filesystems/mount_api.rst-
Documentation/filesystems/mount_api.rst-   * ::
Documentation/filesystems/mount_api.rst-
Documentation/filesystems/mount_api.rst:	int security_fs_context_dup(struct fs_context *fc,
Documentation/filesystems/mount_api.rst-				    struct fs_context *src_fc);
Documentation/filesystems/mount_api.rst-
Documentation/filesystems/mount_api.rst-     Called to initialise fc->security (which is preset to NULL) and allocate
--
fs/fs_context.c-	if (ret < 0)
fs/fs_context.c-		goto err_fc;
fs/fs_context.c-
fs/fs_context.c:	ret = security_fs_context_dup(fc, src_fc);
fs/fs_context.c-	if (ret < 0)
fs/fs_context.c-		goto err_fc;
fs/fs_context.c-	return fc;
```

---

# security hook: security_fs_context_parse_param

Documentation/filesystems/mount_api.rst:1
fs/fs_context.c:1

```shell
Documentation/filesystems/mount_api.rst-
Documentation/filesystems/mount_api.rst-   * ::
Documentation/filesystems/mount_api.rst-
Documentation/filesystems/mount_api.rst:	int security_fs_context_parse_param(struct fs_context *fc,
Documentation/filesystems/mount_api.rst-					    struct fs_parameter *param);
Documentation/filesystems/mount_api.rst-
Documentation/filesystems/mount_api.rst-     Called for each mount parameter, including the source.  The arguments are
--
fs/fs_context.c-	if (ret != -ENOPARAM)
fs/fs_context.c-		return ret;
fs/fs_context.c-
fs/fs_context.c:	ret = security_fs_context_parse_param(fc, param);
fs/fs_context.c-	if (ret != -ENOPARAM)
fs/fs_context.c-		/* Param belongs to the LSM or is disallowed by the LSM; so
fs/fs_context.c-		 * don't pass to the FS.
```

---

# security hook: security_fs_context_submount

fs/fs_context.c:1

```shell
fs/fs_context.c-	if (IS_ERR(fc))
fs/fs_context.c-		return fc;
fs/fs_context.c-
fs/fs_context.c:	ret = security_fs_context_submount(fc, reference->d_sb);
fs/fs_context.c-	if (ret) {
fs/fs_context.c-		put_fs_context(fc);
fs/fs_context.c-		return ERR_PTR(ret);
```

---

# security hook: security_getprocattr

fs/proc/base.c:1

```shell
fs/proc/base.c-	if (!task)
fs/proc/base.c-		return -ESRCH;
fs/proc/base.c-
fs/proc/base.c:	length = security_getprocattr(task, PROC_I(inode)->op.lsm,
fs/proc/base.c-				      file->f_path.dentry->d_name.name,
fs/proc/base.c-				      &p);
fs/proc/base.c-	put_task_struct(task);
```

---

# security hook: security_ib_alloc_security

drivers/infiniband/core/security.c:2

```shell
drivers/infiniband/core/security.c-	INIT_LIST_HEAD(&qp->qp_sec->shared_qp_list);
drivers/infiniband/core/security.c-	atomic_set(&qp->qp_sec->error_list_count, 0);
drivers/infiniband/core/security.c-	init_completion(&qp->qp_sec->error_complete);
drivers/infiniband/core/security.c:	ret = security_ib_alloc_security(&qp->qp_sec->security);
drivers/infiniband/core/security.c-	if (ret) {
drivers/infiniband/core/security.c-		kfree(qp->qp_sec);
drivers/infiniband/core/security.c-		qp->qp_sec = NULL;
--
drivers/infiniband/core/security.c-
drivers/infiniband/core/security.c-	INIT_LIST_HEAD(&agent->mad_agent_sec_list);
drivers/infiniband/core/security.c-
drivers/infiniband/core/security.c:	ret = security_ib_alloc_security(&agent->security);
drivers/infiniband/core/security.c-	if (ret)
drivers/infiniband/core/security.c-		return ret;
drivers/infiniband/core/security.c-
```

---

# security hook: security_ib_endport_manage_subnet

drivers/infiniband/core/security.c:2

```shell
drivers/infiniband/core/security.c-			    &mad_agent_list,
drivers/infiniband/core/security.c-			    mad_agent_sec_list)
drivers/infiniband/core/security.c-		WRITE_ONCE(ag->smp_allowed,
drivers/infiniband/core/security.c:			   !security_ib_endport_manage_subnet(ag->security,
drivers/infiniband/core/security.c-				dev_name(&ag->device->dev), ag->port_num));
drivers/infiniband/core/security.c-	spin_unlock(&mad_agent_list_lock);
drivers/infiniband/core/security.c-}
--
drivers/infiniband/core/security.c-		return 0;
drivers/infiniband/core/security.c-
drivers/infiniband/core/security.c-	spin_lock(&mad_agent_list_lock);
drivers/infiniband/core/security.c:	ret = security_ib_endport_manage_subnet(agent->security,
drivers/infiniband/core/security.c-						dev_name(&agent->device->dev),
drivers/infiniband/core/security.c-						agent->port_num);
drivers/infiniband/core/security.c-	if (ret)
```

---

# security hook: security_ib_free_security

drivers/infiniband/core/security.c:3

```shell
drivers/infiniband/core/security.c-
drivers/infiniband/core/security.c-static void destroy_qp_security(struct ib_qp_security *sec)
drivers/infiniband/core/security.c-{
drivers/infiniband/core/security.c:	security_ib_free_security(sec->security);
drivers/infiniband/core/security.c-	kfree(sec->ports_pkeys);
drivers/infiniband/core/security.c-	kfree(sec);
drivers/infiniband/core/security.c-}
--
drivers/infiniband/core/security.c-
drivers/infiniband/core/security.c-free_security:
drivers/infiniband/core/security.c-	spin_unlock(&mad_agent_list_lock);
drivers/infiniband/core/security.c:	security_ib_free_security(agent->security);
drivers/infiniband/core/security.c-	return ret;
drivers/infiniband/core/security.c-}
drivers/infiniband/core/security.c-
--
drivers/infiniband/core/security.c-		spin_unlock(&mad_agent_list_lock);
drivers/infiniband/core/security.c-	}
drivers/infiniband/core/security.c-
drivers/infiniband/core/security.c:	security_ib_free_security(agent->security);
drivers/infiniband/core/security.c-}
drivers/infiniband/core/security.c-
drivers/infiniband/core/security.c-int ib_mad_enforce_security(struct ib_mad_agent_private *map, u16 pkey_index)
```

---

# security hook: security_ib_pkey_access

drivers/infiniband/core/security.c:3

```shell
drivers/infiniband/core/security.c-	struct ib_qp_security *shared_qp_sec;
drivers/infiniband/core/security.c-	int ret;
drivers/infiniband/core/security.c-
drivers/infiniband/core/security.c:	ret = security_ib_pkey_access(qp_sec->security, subnet_prefix, pkey);
drivers/infiniband/core/security.c-	if (ret)
drivers/infiniband/core/security.c-		return ret;
drivers/infiniband/core/security.c-
drivers/infiniband/core/security.c-	list_for_each_entry(shared_qp_sec,
drivers/infiniband/core/security.c-			    &qp_sec->shared_qp_list,
drivers/infiniband/core/security.c-			    shared_qp_list) {
drivers/infiniband/core/security.c:		ret = security_ib_pkey_access(shared_qp_sec->security,
drivers/infiniband/core/security.c-					      subnet_prefix,
drivers/infiniband/core/security.c-					      pkey);
drivers/infiniband/core/security.c-		if (ret)
--
drivers/infiniband/core/security.c-
drivers/infiniband/core/security.c-	ib_get_cached_subnet_prefix(dev, port_num, &subnet_prefix);
drivers/infiniband/core/security.c-
drivers/infiniband/core/security.c:	return security_ib_pkey_access(sec, subnet_prefix, pkey);
drivers/infiniband/core/security.c-}
drivers/infiniband/core/security.c-
drivers/infiniband/core/security.c-void ib_mad_agent_security_change(void)
```

---

# security hook: security_inet_conn_established

net/ipv4/tcp_input.c:1

```shell
net/ipv4/tcp_input.c-
net/ipv4/tcp_input.c-	if (skb) {
net/ipv4/tcp_input.c-		icsk->icsk_af_ops->sk_rx_dst_set(sk, skb);
net/ipv4/tcp_input.c:		security_inet_conn_established(sk, skb);
net/ipv4/tcp_input.c-		sk_mark_napi_id(sk, skb);
net/ipv4/tcp_input.c-	}
net/ipv4/tcp_input.c-
```

---

# security hook: security_inet_conn_request

net/dccp/ipv6.c:1
net/dccp/ipv4.c:1
net/ipv4/syncookies.c:1
net/ipv4/tcp_ipv4.c:1
net/ipv6/syncookies.c:1
net/ipv6/tcp_ipv6.c:1

```shell
net/dccp/ipv6.c-	ireq->ireq_family = AF_INET6;
net/dccp/ipv6.c-	ireq->ir_mark = inet_request_mark(sk, skb);
net/dccp/ipv6.c-
net/dccp/ipv6.c:	if (security_inet_conn_request(sk, skb, req))
net/dccp/ipv6.c-		goto drop_and_free;
net/dccp/ipv6.c-
net/dccp/ipv6.c-	if (ipv6_opt_accepted(sk, skb, IP6CB(skb)) ||
--
net/dccp/ipv4.c-	ireq->ireq_family = AF_INET;
net/dccp/ipv4.c-	ireq->ir_iif = READ_ONCE(sk->sk_bound_dev_if);
net/dccp/ipv4.c-
net/dccp/ipv4.c:	if (security_inet_conn_request(sk, skb, req))
net/dccp/ipv4.c-		goto drop_and_free;
net/dccp/ipv4.c-
net/dccp/ipv4.c-	/*
--
net/ipv4/syncookies.c-	 */
net/ipv4/syncookies.c-	RCU_INIT_POINTER(ireq->ireq_opt, tcp_v4_save_options(sock_net(sk), skb));
net/ipv4/syncookies.c-
net/ipv4/syncookies.c:	if (security_inet_conn_request(sk, skb, req)) {
net/ipv4/syncookies.c-		reqsk_free(req);
net/ipv4/syncookies.c-		goto out;
net/ipv4/syncookies.c-	}
--
net/ipv4/tcp_ipv4.c-{
net/ipv4/tcp_ipv4.c-	tcp_v4_init_req(req, sk, skb);
net/ipv4/tcp_ipv4.c-
net/ipv4/tcp_ipv4.c:	if (security_inet_conn_request(sk, skb, req))
net/ipv4/tcp_ipv4.c-		return NULL;
net/ipv4/tcp_ipv4.c-
net/ipv4/tcp_ipv4.c-	return inet_csk_route_req(sk, &fl->u.ip4, req);
--
net/ipv6/syncookies.c-	ireq->ir_v6_rmt_addr = ipv6_hdr(skb)->saddr;
net/ipv6/syncookies.c-	ireq->ir_v6_loc_addr = ipv6_hdr(skb)->daddr;
net/ipv6/syncookies.c-
net/ipv6/syncookies.c:	if (security_inet_conn_request(sk, skb, req))
net/ipv6/syncookies.c-		goto out_free;
net/ipv6/syncookies.c-
net/ipv6/syncookies.c-	if (ipv6_opt_accepted(sk, skb, &TCP_SKB_CB(skb)->header.h6) ||
--
net/ipv6/tcp_ipv6.c-{
net/ipv6/tcp_ipv6.c-	tcp_v6_init_req(req, sk, skb);
net/ipv6/tcp_ipv6.c-
net/ipv6/tcp_ipv6.c:	if (security_inet_conn_request(sk, skb, req))
net/ipv6/tcp_ipv6.c-		return NULL;
net/ipv6/tcp_ipv6.c-
net/ipv6/tcp_ipv6.c-	return inet6_csk_route_req(sk, &fl->u.ip6, req, IPPROTO_TCP);
```

---

# security hook: security_inet_csk_clone

net/ipv4/inet_connection_sock.c:1
net/mptcp/protocol.c:1

```shell
net/ipv4/inet_connection_sock.c-
net/ipv4/inet_connection_sock.c-		inet_clone_ulp(req, newsk, priority);
net/ipv4/inet_connection_sock.c-
net/ipv4/inet_connection_sock.c:		security_inet_csk_clone(newsk, req);
net/ipv4/inet_connection_sock.c-	}
net/ipv4/inet_connection_sock.c-	return newsk;
net/ipv4/inet_connection_sock.c-}
--
net/mptcp/protocol.c-	msk->subflow_id = 2;
net/mptcp/protocol.c-
net/mptcp/protocol.c-	sock_reset_flag(nsk, SOCK_RCU_FREE);
net/mptcp/protocol.c:	security_inet_csk_clone(nsk, req);
net/mptcp/protocol.c-
net/mptcp/protocol.c-	/* this can't race with mptcp_close(), as the msk is
net/mptcp/protocol.c-	 * not yet exposted to user-space
```

---

# security hook: security_init

net/ipv4/netfilter/iptable_security.c:2
net/ipv6/netfilter/ip6table_security.c:2
fs/ceph/inode.c:1
fs/ceph/super.h:2
fs/ceph/xattr.c:1
fs/ntfs3/super.c:1
fs/ntfs3/ntfs_fs.h:1
fs/ntfs3/fsntfs.c:2
fs/btrfs/inode.c:1
fs/btrfs/xattr.c:1
fs/reiserfs/xattr_security.c:1
fs/btrfs/xattr.h:1
fs/reiserfs/xattr.h:2
fs/reiserfs/namei.c:4
fs/ocfs2/xattr.c:1
fs/ocfs2/xattr.h:1
fs/ocfs2/namei.c:1
tools/testing/nvdimm/test/nfit.c:2
init/main.c:2
drivers/net/wireless/broadcom/b43legacy/main.c:2
drivers/net/wireless/broadcom/b43/main.c:2

```shell
net/ipv6/netfilter/ip6table_security.c-	.exit = ip6table_security_net_exit,
net/ipv6/netfilter/ip6table_security.c-};
net/ipv6/netfilter/ip6table_security.c-
net/ipv6/netfilter/ip6table_security.c:static int __init ip6table_security_init(void)
net/ipv6/netfilter/ip6table_security.c-{
net/ipv6/netfilter/ip6table_security.c-	int ret = xt_register_template(&security_table,
net/ipv6/netfilter/ip6table_security.c-				       ip6table_security_table_init);
--
net/ipv6/netfilter/ip6table_security.c-	kfree(sectbl_ops);
net/ipv6/netfilter/ip6table_security.c-}
net/ipv6/netfilter/ip6table_security.c-
net/ipv6/netfilter/ip6table_security.c:module_init(ip6table_security_init);
net/ipv6/netfilter/ip6table_security.c-module_exit(ip6table_security_fini);
--
net/ipv4/netfilter/iptable_security.c-	.exit = iptable_security_net_exit,
net/ipv4/netfilter/iptable_security.c-};
net/ipv4/netfilter/iptable_security.c-
net/ipv4/netfilter/iptable_security.c:static int __init iptable_security_init(void)
net/ipv4/netfilter/iptable_security.c-{
net/ipv4/netfilter/iptable_security.c-	int ret = xt_register_template(&security_table,
net/ipv4/netfilter/iptable_security.c-				       iptable_security_table_init);
--
net/ipv4/netfilter/iptable_security.c-	xt_unregister_template(&security_table);
net/ipv4/netfilter/iptable_security.c-}
net/ipv4/netfilter/iptable_security.c-
net/ipv4/netfilter/iptable_security.c:module_init(iptable_security_init);
net/ipv4/netfilter/iptable_security.c-module_exit(iptable_security_fini);
--
tools/testing/nvdimm/test/nfit.c-	return 0;
tools/testing/nvdimm/test/nfit.c-}
tools/testing/nvdimm/test/nfit.c-
tools/testing/nvdimm/test/nfit.c:static void nfit_security_init(struct nfit_test *t)
tools/testing/nvdimm/test/nfit.c-{
tools/testing/nvdimm/test/nfit.c-	int i;
tools/testing/nvdimm/test/nfit.c-
--
tools/testing/nvdimm/test/nfit.c-	if (nfit_test_dimm_init(t))
tools/testing/nvdimm/test/nfit.c-		return -ENOMEM;
tools/testing/nvdimm/test/nfit.c-	smart_init(t);
tools/testing/nvdimm/test/nfit.c:	nfit_security_init(t);
tools/testing/nvdimm/test/nfit.c-	return ars_state_init(&t->pdev.dev, &t->ars_state);
tools/testing/nvdimm/test/nfit.c-}
tools/testing/nvdimm/test/nfit.c-
--
fs/ceph/inode.c-	inode->i_state = 0;
fs/ceph/inode.c-	inode->i_mode = *mode;
fs/ceph/inode.c-
fs/ceph/inode.c:	err = ceph_security_init_secctx(dentry, *mode, as_ctx);
fs/ceph/inode.c-	if (err < 0)
fs/ceph/inode.c-		goto out_err;
fs/ceph/inode.c-
--
fs/ceph/super.h-#endif
fs/ceph/super.h-
fs/ceph/super.h-#ifdef CONFIG_CEPH_FS_SECURITY_LABEL
fs/ceph/super.h:extern int ceph_security_init_secctx(struct dentry *dentry, umode_t mode,
fs/ceph/super.h-				     struct ceph_acl_sec_ctx *ctx);
fs/ceph/super.h-static inline void ceph_security_invalidate_secctx(struct inode *inode)
fs/ceph/super.h-{
fs/ceph/super.h-	security_inode_invalidate_secctx(inode);
fs/ceph/super.h-}
fs/ceph/super.h-#else
fs/ceph/super.h:static inline int ceph_security_init_secctx(struct dentry *dentry, umode_t mode,
fs/ceph/super.h-					    struct ceph_acl_sec_ctx *ctx)
fs/ceph/super.h-{
fs/ceph/super.h-	return 0;
--
fs/ceph/xattr.c-}
fs/ceph/xattr.c-
fs/ceph/xattr.c-#ifdef CONFIG_CEPH_FS_SECURITY_LABEL
fs/ceph/xattr.c:int ceph_security_init_secctx(struct dentry *dentry, umode_t mode,
fs/ceph/xattr.c-			   struct ceph_acl_sec_ctx *as_ctx)
fs/ceph/xattr.c-{
fs/ceph/xattr.c-	struct ceph_pagelist *pagelist = as_ctx->pagelist;
--
fs/ntfs3/super.c-
fs/ntfs3/super.c-	if (is_ntfs3(sbi)) {
fs/ntfs3/super.c-		/* Load $Secure. */
fs/ntfs3/super.c:		err = ntfs_security_init(sbi);
fs/ntfs3/super.c-		if (err) {
fs/ntfs3/super.c-			ntfs_err(sb, "Failed to initialize $Secure (%d).", err);
fs/ntfs3/super.c-			goto out;
--
fs/ntfs3/ntfs_fs.h-				  enum RECORD_FLAG flag);
fs/ntfs3/ntfs_fs.h-extern const u8 s_default_security[0x50];
fs/ntfs3/ntfs_fs.h-bool is_sd_valid(const struct SECURITY_DESCRIPTOR_RELATIVE *sd, u32 len);
fs/ntfs3/ntfs_fs.h:int ntfs_security_init(struct ntfs_sb_info *sbi);
fs/ntfs3/ntfs_fs.h-int ntfs_get_security_by_id(struct ntfs_sb_info *sbi, __le32 security_id,
fs/ntfs3/ntfs_fs.h-			    struct SECURITY_DESCRIPTOR_RELATIVE **sd,
fs/ntfs3/ntfs_fs.h-			    size_t *size);
--
fs/ntfs3/fsntfs.c-}
fs/ntfs3/fsntfs.c-
fs/ntfs3/fsntfs.c-/*
fs/ntfs3/fsntfs.c: * ntfs_security_init - Load and parse $Secure.
fs/ntfs3/fsntfs.c- */
fs/ntfs3/fsntfs.c:int ntfs_security_init(struct ntfs_sb_info *sbi)
fs/ntfs3/fsntfs.c-{
fs/ntfs3/fsntfs.c-	int err;
fs/ntfs3/fsntfs.c-	struct super_block *sb = sbi->sb;
--
fs/reiserfs/xattr_security.c-/* Initializes the security context for a new inode and returns the number
fs/reiserfs/xattr_security.c- * of blocks needed for the transaction. If successful, reiserfs_security
fs/reiserfs/xattr_security.c- * must be released using reiserfs_security_free when the caller is done. */
fs/reiserfs/xattr_security.c:int reiserfs_security_init(struct inode *dir, struct inode *inode,
fs/reiserfs/xattr_security.c-			   const struct qstr *qstr,
fs/reiserfs/xattr_security.c-			   struct reiserfs_security_handle *sec)
fs/reiserfs/xattr_security.c-{
--
fs/reiserfs/xattr.h-extern const struct xattr_handler reiserfs_xattr_trusted_handler;
fs/reiserfs/xattr.h-extern const struct xattr_handler reiserfs_xattr_security_handler;
fs/reiserfs/xattr.h-#ifdef CONFIG_REISERFS_FS_SECURITY
fs/reiserfs/xattr.h:int reiserfs_security_init(struct inode *dir, struct inode *inode,
fs/reiserfs/xattr.h-			   const struct qstr *qstr,
fs/reiserfs/xattr.h-			   struct reiserfs_security_handle *sec);
fs/reiserfs/xattr.h-int reiserfs_security_write(struct reiserfs_transaction_handle *th,
--
fs/reiserfs/xattr.h-#endif  /*  CONFIG_REISERFS_FS_XATTR  */
fs/reiserfs/xattr.h-
fs/reiserfs/xattr.h-#ifndef CONFIG_REISERFS_FS_SECURITY
fs/reiserfs/xattr.h:static inline int reiserfs_security_init(struct inode *dir,
fs/reiserfs/xattr.h-					 struct inode *inode,
fs/reiserfs/xattr.h-					 const struct qstr *qstr,
fs/reiserfs/xattr.h-					 struct reiserfs_security_handle *sec)
--
fs/reiserfs/namei.c-	}
fs/reiserfs/namei.c-
fs/reiserfs/namei.c-	jbegin_count += reiserfs_cache_default_acl(dir);
fs/reiserfs/namei.c:	retval = reiserfs_security_init(dir, inode, &dentry->d_name, &security);
fs/reiserfs/namei.c-	if (retval < 0) {
fs/reiserfs/namei.c-		drop_new_inode(inode);
fs/reiserfs/namei.c-		return retval;
--
fs/reiserfs/namei.c-	}
fs/reiserfs/namei.c-
fs/reiserfs/namei.c-	jbegin_count += reiserfs_cache_default_acl(dir);
fs/reiserfs/namei.c:	retval = reiserfs_security_init(dir, inode, &dentry->d_name, &security);
fs/reiserfs/namei.c-	if (retval < 0) {
fs/reiserfs/namei.c-		drop_new_inode(inode);
fs/reiserfs/namei.c-		return retval;
--
fs/reiserfs/namei.c-	}
fs/reiserfs/namei.c-
fs/reiserfs/namei.c-	jbegin_count += reiserfs_cache_default_acl(dir);
fs/reiserfs/namei.c:	retval = reiserfs_security_init(dir, inode, &dentry->d_name, &security);
fs/reiserfs/namei.c-	if (retval < 0) {
fs/reiserfs/namei.c-		drop_new_inode(inode);
fs/reiserfs/namei.c-		return retval;
--
fs/reiserfs/namei.c-		return retval;
fs/reiserfs/namei.c-	}
fs/reiserfs/namei.c-
fs/reiserfs/namei.c:	retval = reiserfs_security_init(parent_dir, inode, &dentry->d_name,
fs/reiserfs/namei.c-					&security);
fs/reiserfs/namei.c-	if (retval < 0) {
fs/reiserfs/namei.c-		drop_new_inode(inode);
--
fs/ocfs2/xattr.c-		sizeof(struct ocfs2_xattr_entry);
fs/ocfs2/xattr.c-}
fs/ocfs2/xattr.c-
fs/ocfs2/xattr.c:int ocfs2_calc_security_init(struct inode *dir,
fs/ocfs2/xattr.c-			     struct ocfs2_security_xattr_info *si,
fs/ocfs2/xattr.c-			     int *want_clusters,
fs/ocfs2/xattr.c-			     int *xattr_credits,
--
fs/ocfs2/xattr.h-			    struct ocfs2_security_xattr_info *,
fs/ocfs2/xattr.h-			    struct ocfs2_alloc_context *,
fs/ocfs2/xattr.h-			    struct ocfs2_alloc_context *);
fs/ocfs2/xattr.h:int ocfs2_calc_security_init(struct inode *,
fs/ocfs2/xattr.h-			     struct ocfs2_security_xattr_info *,
fs/ocfs2/xattr.h-			     int *, int *, struct ocfs2_alloc_context **);
fs/ocfs2/xattr.h-int ocfs2_calc_xattr_init(struct inode *, struct buffer_head *,
--
fs/ocfs2/namei.c-
fs/ocfs2/namei.c-	/* calculate meta data/clusters for setting security xattr */
fs/ocfs2/namei.c-	if (si.enable) {
fs/ocfs2/namei.c:		status = ocfs2_calc_security_init(dir, &si, &want_clusters,
fs/ocfs2/namei.c-						  &xattr_credits, &xattr_ac);
fs/ocfs2/namei.c-		if (status < 0) {
fs/ocfs2/namei.c-			mlog_errno(status);
--
fs/btrfs/inode.c-	}
fs/btrfs/inode.c-	if (!args->default_acl && !args->acl)
fs/btrfs/inode.c-		cache_no_acl(args->inode);
fs/btrfs/inode.c:	return btrfs_xattr_security_init(trans, args->inode, args->dir,
fs/btrfs/inode.c-					 &args->dentry->d_name);
fs/btrfs/inode.c-}
fs/btrfs/inode.c-
--
fs/btrfs/xattr.c-	return err;
fs/btrfs/xattr.c-}
fs/btrfs/xattr.c-
fs/btrfs/xattr.c:int btrfs_xattr_security_init(struct btrfs_trans_handle *trans,
fs/btrfs/xattr.c-			      struct inode *inode, struct inode *dir,
fs/btrfs/xattr.c-			      const struct qstr *qstr)
fs/btrfs/xattr.c-{
--
fs/btrfs/xattr.h-			 const void *value, size_t size, int flags);
fs/btrfs/xattr.h-ssize_t btrfs_listxattr(struct dentry *dentry, char *buffer, size_t size);
fs/btrfs/xattr.h-
fs/btrfs/xattr.h:int btrfs_xattr_security_init(struct btrfs_trans_handle *trans,
fs/btrfs/xattr.h-				     struct inode *inode, struct inode *dir,
fs/btrfs/xattr.h-				     const struct qstr *qstr);
fs/btrfs/xattr.h-
--
drivers/net/wireless/broadcom/b43legacy/main.c-	return -ENODEV;
drivers/net/wireless/broadcom/b43legacy/main.c-}
drivers/net/wireless/broadcom/b43legacy/main.c-
drivers/net/wireless/broadcom/b43legacy/main.c:static void b43legacy_security_init(struct b43legacy_wldev *dev)
drivers/net/wireless/broadcom/b43legacy/main.c-{
drivers/net/wireless/broadcom/b43legacy/main.c-	dev->max_nr_keys = (dev->dev->id.revision >= 5) ? 58 : 20;
drivers/net/wireless/broadcom/b43legacy/main.c-	B43legacy_WARN_ON(dev->max_nr_keys > ARRAY_SIZE(dev->key));
--
drivers/net/wireless/broadcom/b43legacy/main.c-
drivers/net/wireless/broadcom/b43legacy/main.c-	ssb_bus_powerup(bus, 1); /* Enable dynamic PCTL */
drivers/net/wireless/broadcom/b43legacy/main.c-	b43legacy_upload_card_macaddress(dev);
drivers/net/wireless/broadcom/b43legacy/main.c:	b43legacy_security_init(dev);
drivers/net/wireless/broadcom/b43legacy/main.c-	b43legacy_rng_init(wl);
drivers/net/wireless/broadcom/b43legacy/main.c-
drivers/net/wireless/broadcom/b43legacy/main.c-	ieee80211_wake_queues(dev->wl->hw);
--
drivers/net/wireless/broadcom/b43/main.c-	return -ENODEV;
drivers/net/wireless/broadcom/b43/main.c-}
drivers/net/wireless/broadcom/b43/main.c-
drivers/net/wireless/broadcom/b43/main.c:static void b43_security_init(struct b43_wldev *dev)
drivers/net/wireless/broadcom/b43/main.c-{
drivers/net/wireless/broadcom/b43/main.c-	dev->ktp = b43_shm_read16(dev, B43_SHM_SHARED, B43_SHM_SH_KTP);
drivers/net/wireless/broadcom/b43/main.c-	/* KTP is a word address, but we address SHM bytewise.
--
drivers/net/wireless/broadcom/b43/main.c-
drivers/net/wireless/broadcom/b43/main.c-	b43_bus_powerup(dev, !(sprom->boardflags_lo & B43_BFL_XTAL_NOSLOW));
drivers/net/wireless/broadcom/b43/main.c-	b43_upload_card_macaddress(dev);
drivers/net/wireless/broadcom/b43/main.c:	b43_security_init(dev);
drivers/net/wireless/broadcom/b43/main.c-
drivers/net/wireless/broadcom/b43/main.c-	ieee80211_wake_queues(dev->wl->hw);
drivers/net/wireless/broadcom/b43/main.c-
--
init/main.c-	boot_cpu_init();
init/main.c-	page_address_init();
init/main.c-	pr_notice("%s", linux_banner);
init/main.c:	early_security_init();
init/main.c-	setup_arch(&command_line);
init/main.c-	setup_boot_config();
init/main.c-	setup_command_line(command_line);
--
init/main.c-	proc_caches_init();
init/main.c-	uts_ns_init();
init/main.c-	key_init();
init/main.c:	security_init();
init/main.c-	dbg_late_init();
init/main.c-	net_ns_init();
init/main.c-	vfs_caches_init();
```

---

# security hook: security_inode_alloc

Documentation/security/lsm.rst:1
fs/inode.c:1

```shell
Documentation/security/lsm.rst-The hooks can be viewed as falling into two major
Documentation/security/lsm.rst-categories: hooks that are used to manage the security fields and hooks
Documentation/security/lsm.rst-that are used to perform access control. Examples of the first category
Documentation/security/lsm.rst:of hooks include the security_inode_alloc() and security_inode_free()
Documentation/security/lsm.rst-These hooks are used to allocate
Documentation/security/lsm.rst-and free security structures for inode objects.
Documentation/security/lsm.rst-An example of the second category of hooks
--
fs/inode.c-#endif
fs/inode.c-	inode->i_flctx = NULL;
fs/inode.c-
fs/inode.c:	if (unlikely(security_inode_alloc(inode)))
fs/inode.c-		return -ENOMEM;
fs/inode.c-	this_cpu_inc(nr_inodes);
fs/inode.c-
```

---

# security hook: security_inode_copy_up

fs/overlayfs/copy_up.c:2

```shell
fs/overlayfs/copy_up.c-		if (ovl_is_private_xattr(sb, name))
fs/overlayfs/copy_up.c-			continue;
fs/overlayfs/copy_up.c-
fs/overlayfs/copy_up.c:		error = security_inode_copy_up_xattr(name);
fs/overlayfs/copy_up.c-		if (error < 0 && error != -EOPNOTSUPP)
fs/overlayfs/copy_up.c-			break;
fs/overlayfs/copy_up.c-		if (error == 1) {
--
fs/overlayfs/copy_up.c-	int err;
fs/overlayfs/copy_up.c-
fs/overlayfs/copy_up.c-	cc->old = cc->new = NULL;
fs/overlayfs/copy_up.c:	err = security_inode_copy_up(dentry, &cc->new);
fs/overlayfs/copy_up.c-	if (err < 0)
fs/overlayfs/copy_up.c-		return err;
fs/overlayfs/copy_up.c-
```

---

# security hook: security_inode_copy_up_xattr

fs/overlayfs/copy_up.c:1

```shell
fs/overlayfs/copy_up.c-		if (ovl_is_private_xattr(sb, name))
fs/overlayfs/copy_up.c-			continue;
fs/overlayfs/copy_up.c-
fs/overlayfs/copy_up.c:		error = security_inode_copy_up_xattr(name);
fs/overlayfs/copy_up.c-		if (error < 0 && error != -EOPNOTSUPP)
fs/overlayfs/copy_up.c-			break;
fs/overlayfs/copy_up.c-		if (error == 1) {
```

---

# security hook: security_inode_create

fs/cachefiles/security.c:1
fs/namei.c:3

```shell
fs/cachefiles/security.c-		return ret;
fs/cachefiles/security.c-	}
fs/cachefiles/security.c-
fs/cachefiles/security.c:	ret = security_inode_create(d_backing_inode(root), root, 0);
fs/cachefiles/security.c-	if (ret < 0)
fs/cachefiles/security.c-		pr_err("Security denies permission to create files: error %d",
fs/cachefiles/security.c-		       ret);
--
fs/namei.c-		return -EACCES;	/* shouldn't it be ENOSYS? */
fs/namei.c-
fs/namei.c-	mode = vfs_prepare_mode(idmap, dir, mode, S_IALLUGO, S_IFREG);
fs/namei.c:	error = security_inode_create(dir, dentry, mode);
fs/namei.c-	if (error)
fs/namei.c-		return error;
fs/namei.c-	error = dir->i_op->create(idmap, dir, dentry, mode, want_excl);
--
fs/namei.c-
fs/namei.c-	mode &= S_IALLUGO;
fs/namei.c-	mode |= S_IFREG;
fs/namei.c:	error = security_inode_create(dir, dentry, mode);
fs/namei.c-	if (error)
fs/namei.c-		return error;
fs/namei.c-	error = f(dentry, mode, arg);
--
fs/namei.c-	if (error)
fs/namei.c-		return error;
fs/namei.c-
fs/namei.c:	return security_inode_create(dir->dentry->d_inode, dentry, mode);
fs/namei.c-}
fs/namei.c-
fs/namei.c-/*
```

---

# security hook: security_inode_follow_link

fs/overlayfs/inode.c:1
fs/namei.c:1

```shell
fs/overlayfs/inode.c- * > This is a similar situation as reading a symlink vs. following it.
fs/overlayfs/inode.c- * > When following a symlink overlayfs always reads the link on the
fs/overlayfs/inode.c- * > underlying fs just as if it was a readlink(2) call, calling
fs/overlayfs/inode.c: * > security_inode_readlink() instead of security_inode_follow_link().
fs/overlayfs/inode.c- * > This is logical: we are reading the link from the underlying storage,
fs/overlayfs/inode.c- * > and following it on overlayfs.
fs/overlayfs/inode.c- * >
--
fs/namei.c-		touch_atime(&last->link);
fs/namei.c-	}
fs/namei.c-
fs/namei.c:	error = security_inode_follow_link(link->dentry, inode,
fs/namei.c-					   nd->flags & LOOKUP_RCU);
fs/namei.c-	if (unlikely(error))
fs/namei.c-		return ERR_PTR(error);
```

---

# security hook: security_inode_free

Documentation/security/lsm.rst:1
fs/inode.c:1

```shell
Documentation/security/lsm.rst-The hooks can be viewed as falling into two major
Documentation/security/lsm.rst-categories: hooks that are used to manage the security fields and hooks
Documentation/security/lsm.rst-that are used to perform access control. Examples of the first category
Documentation/security/lsm.rst:of hooks include the security_inode_alloc() and security_inode_free()
Documentation/security/lsm.rst-These hooks are used to allocate
Documentation/security/lsm.rst-and free security structures for inode objects.
Documentation/security/lsm.rst-An example of the second category of hooks
--
fs/inode.c-{
fs/inode.c-	BUG_ON(inode_has_buffers(inode));
fs/inode.c-	inode_detach_wb(inode);
fs/inode.c:	security_inode_free(inode);
fs/inode.c-	fsnotify_inode_delete(inode);
fs/inode.c-	locks_free_lock_context(inode);
fs/inode.c-	if (!inode->i_nlink) {
```

---

# security hook: security_inode_get_acl

fs/posix_acl.c:1

```shell
fs/posix_acl.c-	 * The VFS has no restrictions on reading POSIX ACLs so calling
fs/posix_acl.c-	 * something like xattr_permission() isn't needed. Only LSMs get a say.
fs/posix_acl.c-	 */
fs/posix_acl.c:	error = security_inode_get_acl(idmap, dentry, acl_name);
fs/posix_acl.c-	if (error)
fs/posix_acl.c-		return ERR_PTR(error);
fs/posix_acl.c-
```

---

# security hook: security_inode_getattr

kernel/trace/bpf_trace.c:1
fs/stat.c:2
tools/testing/selftests/bpf/prog_tests/d_path.c:1
tools/testing/selftests/bpf/progs/test_d_path_check_types.c:1
tools/testing/selftests/bpf/progs/test_d_path_check_rdonly_mem.c:1
tools/testing/selftests/bpf/progs/test_d_path.c:1

```shell
kernel/trace/bpf_trace.c-BTF_SET_START(btf_allowlist_d_path)
kernel/trace/bpf_trace.c-#ifdef CONFIG_SECURITY
kernel/trace/bpf_trace.c-BTF_ID(func, security_file_permission)
kernel/trace/bpf_trace.c:BTF_ID(func, security_inode_getattr)
kernel/trace/bpf_trace.c-BTF_ID(func, security_file_open)
kernel/trace/bpf_trace.c-#endif
kernel/trace/bpf_trace.c-#ifdef CONFIG_SECURITY_PATH
--
fs/stat.c- * @request_mask: STATX_xxx flags indicating what the caller wants
fs/stat.c- * @query_flags: Query mode (AT_STATX_SYNC_TYPE)
fs/stat.c- *
fs/stat.c: * Get attributes without calling security_inode_getattr.
fs/stat.c- *
fs/stat.c- * Currently the only caller other than vfs_getattr is internal to the
fs/stat.c- * filehandle lookup code, which uses only the inode number and returns no
--
fs/stat.c-	if (WARN_ON_ONCE(query_flags & AT_GETATTR_NOSEC))
fs/stat.c-		return -EPERM;
fs/stat.c-
fs/stat.c:	retval = security_inode_getattr(path);
fs/stat.c-	if (retval)
fs/stat.c-		return retval;
fs/stat.c-	return vfs_getattr_nosec(path, stat, request_mask, query_flags);
--
tools/testing/selftests/bpf/prog_tests/d_path.c-
tools/testing/selftests/bpf/prog_tests/d_path.c-	if (CHECK(!bss->called_stat,
tools/testing/selftests/bpf/prog_tests/d_path.c-		  "stat",
tools/testing/selftests/bpf/prog_tests/d_path.c:		  "trampoline for security_inode_getattr was not called\n"))
tools/testing/selftests/bpf/prog_tests/d_path.c-		goto cleanup;
tools/testing/selftests/bpf/prog_tests/d_path.c-
tools/testing/selftests/bpf/prog_tests/d_path.c-	if (CHECK(!bss->called_close,
--
tools/testing/selftests/bpf/progs/test_d_path_check_types.c-	__uint(max_entries, 1 << 12);
tools/testing/selftests/bpf/progs/test_d_path_check_types.c-} ringbuf SEC(".maps");
tools/testing/selftests/bpf/progs/test_d_path_check_types.c-
tools/testing/selftests/bpf/progs/test_d_path_check_types.c:SEC("fentry/security_inode_getattr")
tools/testing/selftests/bpf/progs/test_d_path_check_types.c-int BPF_PROG(d_path_check_rdonly_mem, struct path *path, struct kstat *stat,
tools/testing/selftests/bpf/progs/test_d_path_check_types.c-	     __u32 request_mask, unsigned int query_flags)
tools/testing/selftests/bpf/progs/test_d_path_check_types.c-{
--
tools/testing/selftests/bpf/progs/test_d_path_check_rdonly_mem.c-
tools/testing/selftests/bpf/progs/test_d_path_check_rdonly_mem.c-extern const int bpf_prog_active __ksym;
tools/testing/selftests/bpf/progs/test_d_path_check_rdonly_mem.c-
tools/testing/selftests/bpf/progs/test_d_path_check_rdonly_mem.c:SEC("fentry/security_inode_getattr")
tools/testing/selftests/bpf/progs/test_d_path_check_rdonly_mem.c-int BPF_PROG(d_path_check_rdonly_mem, struct path *path, struct kstat *stat,
tools/testing/selftests/bpf/progs/test_d_path_check_rdonly_mem.c-	     __u32 request_mask, unsigned int query_flags)
tools/testing/selftests/bpf/progs/test_d_path_check_rdonly_mem.c-{
--
tools/testing/selftests/bpf/progs/test_d_path.c-int called_stat = 0;
tools/testing/selftests/bpf/progs/test_d_path.c-int called_close = 0;
tools/testing/selftests/bpf/progs/test_d_path.c-
tools/testing/selftests/bpf/progs/test_d_path.c:SEC("fentry/security_inode_getattr")
tools/testing/selftests/bpf/progs/test_d_path.c-int BPF_PROG(prog_stat, struct path *path, struct kstat *stat,
tools/testing/selftests/bpf/progs/test_d_path.c-	     __u32 request_mask, unsigned int query_flags)
tools/testing/selftests/bpf/progs/test_d_path.c-{
```

---

# security hook: security_inode_getsecctx

fs/nfsd/nfs4xdr.c:1

```shell
fs/nfsd/nfs4xdr.c-	if ((bmval2 & FATTR4_WORD2_SECURITY_LABEL) ||
fs/nfsd/nfs4xdr.c-	     bmval0 & FATTR4_WORD0_SUPPORTED_ATTRS) {
fs/nfsd/nfs4xdr.c-		if (exp->ex_flags & NFSEXP_SECURITY_LABEL)
fs/nfsd/nfs4xdr.c:			err = security_inode_getsecctx(d_inode(dentry),
fs/nfsd/nfs4xdr.c-						&context, &contextlen);
fs/nfsd/nfs4xdr.c-		else
fs/nfsd/nfs4xdr.c-			err = -EOPNOTSUPP;
```

---

# security hook: security_inode_getsecid

kernel/auditsc.c:1

```shell
kernel/auditsc.c-	name->uid   = inode->i_uid;
kernel/auditsc.c-	name->gid   = inode->i_gid;
kernel/auditsc.c-	name->rdev  = inode->i_rdev;
kernel/auditsc.c:	security_inode_getsecid(inode, &name->osid);
kernel/auditsc.c-	if (flags & AUDIT_INODE_NOEVAL) {
kernel/auditsc.c-		name->fcap_ver = -1;
kernel/auditsc.c-		return;
```

---

# security hook: security_inode_getsecurity

fs/xattr.c:2

```shell
fs/xattr.c-	ssize_t len;
fs/xattr.c-
fs/xattr.c-	if (!value || !size) {
fs/xattr.c:		len = security_inode_getsecurity(idmap, inode, name,
fs/xattr.c-						 &buffer, false);
fs/xattr.c-		goto out_noalloc;
fs/xattr.c-	}
fs/xattr.c-
fs/xattr.c:	len = security_inode_getsecurity(idmap, inode, name, &buffer,
fs/xattr.c-					 true);
fs/xattr.c-	if (len < 0)
fs/xattr.c-		return len;
```

---

# security hook: security_inode_getxattr

fs/overlayfs/inode.c:1
fs/xattr.c:1

```shell
fs/overlayfs/inode.c- * > and following it on overlayfs.
fs/overlayfs/inode.c- * >
fs/overlayfs/inode.c- * > Applying the same logic to acl: we do need to call the
fs/overlayfs/inode.c: * > security_inode_getxattr() on the underlying fs, even if just want to
fs/overlayfs/inode.c- * > check permissions on overlay. This is currently not done, which is an
fs/overlayfs/inode.c- * > inconsistency.
fs/overlayfs/inode.c- * >
--
fs/xattr.c-	if (error)
fs/xattr.c-		return error;
fs/xattr.c-
fs/xattr.c:	error = security_inode_getxattr(dentry, name);
fs/xattr.c-	if (error)
fs/xattr.c-		return error;
fs/xattr.c-
```

---

# security hook: security_inode_init_security

mm/secretmem.c:1
mm/shmem.c:4
fs/xfs/xfs_iops.c:1
fs/gfs2/inode.c:1
fs/ubifs/xattr.c:1
fs/anon_inodes.c:1
fs/f2fs/xattr.c:1
fs/jfs/xattr.c:1
fs/reiserfs/xattr_security.c:1
fs/ext2/xattr_security.c:1
fs/btrfs/xattr.c:1
fs/ext4/xattr_security.c:1
fs/jffs2/security.c:1
fs/ocfs2/xattr.c:3
fs/hfsplus/xattr_security.c:1

```shell
mm/secretmem.c-	 * bypassed for secretmem file descriptors.
mm/secretmem.c-	 */
mm/secretmem.c-	inode->i_flags &= ~S_PRIVATE;
mm/secretmem.c:	err = security_inode_init_security_anon(inode, &qname, NULL);
mm/secretmem.c-	if (err) {
mm/secretmem.c-		file = ERR_PTR(err);
mm/secretmem.c-		goto err_free_inode;
--
mm/shmem.c-	error = simple_acl_create(dir, inode);
mm/shmem.c-	if (error)
mm/shmem.c-		goto out_iput;
mm/shmem.c:	error = security_inode_init_security(inode, dir, &dentry->d_name,
mm/shmem.c-					     shmem_initxattrs, NULL);
mm/shmem.c-	if (error && error != -EOPNOTSUPP)
mm/shmem.c-		goto out_iput;
--
mm/shmem.c-		error = PTR_ERR(inode);
mm/shmem.c-		goto err_out;
mm/shmem.c-	}
mm/shmem.c:	error = security_inode_init_security(inode, dir, NULL,
mm/shmem.c-					     shmem_initxattrs, NULL);
mm/shmem.c-	if (error && error != -EOPNOTSUPP)
mm/shmem.c-		goto out_iput;
--
mm/shmem.c-	if (IS_ERR(inode))
mm/shmem.c-		return PTR_ERR(inode);
mm/shmem.c-
mm/shmem.c:	error = security_inode_init_security(inode, dir, &dentry->d_name,
mm/shmem.c-					     shmem_initxattrs, NULL);
mm/shmem.c-	if (error && error != -EOPNOTSUPP)
mm/shmem.c-		goto out_iput;
--
mm/shmem.c- */
mm/shmem.c-
mm/shmem.c-/*
mm/shmem.c: * Callback for security_inode_init_security() for acquiring xattrs.
mm/shmem.c- */
mm/shmem.c-static int shmem_initxattrs(struct inode *inode,
mm/shmem.c-			    const struct xattr *xattr_array, void *fs_info)
--
fs/xfs/xfs_iops.c-	struct inode	*dir,
fs/xfs/xfs_iops.c-	const struct qstr *qstr)
fs/xfs/xfs_iops.c-{
fs/xfs/xfs_iops.c:	return security_inode_init_security(inode, dir, qstr,
fs/xfs/xfs_iops.c-					     &xfs_initxattrs, NULL);
fs/xfs/xfs_iops.c-}
fs/xfs/xfs_iops.c-
--
fs/gfs2/inode.c-		acl = NULL;
fs/gfs2/inode.c-	}
fs/gfs2/inode.c-
fs/gfs2/inode.c:	error = security_inode_init_security(&ip->i_inode, &dip->i_inode, name,
fs/gfs2/inode.c-					     &gfs2_initxattrs, NULL);
fs/gfs2/inode.c-	if (error)
fs/gfs2/inode.c-		goto fail_gunlock4;
--
fs/ubifs/xattr.c-{
fs/ubifs/xattr.c-	int err;
fs/ubifs/xattr.c-
fs/ubifs/xattr.c:	err = security_inode_init_security(inode, dentry, qstr,
fs/ubifs/xattr.c-					   &init_xattrs, NULL);
fs/ubifs/xattr.c-	if (err) {
fs/ubifs/xattr.c-		struct ubifs_info *c = dentry->i_sb->s_fs_info;
--
fs/anon_inodes.c-	if (IS_ERR(inode))
fs/anon_inodes.c-		return inode;
fs/anon_inodes.c-	inode->i_flags &= ~S_PRIVATE;
fs/anon_inodes.c:	error =	security_inode_init_security_anon(inode, &qname, context_inode);
fs/anon_inodes.c-	if (error) {
fs/anon_inodes.c-		iput(inode);
fs/anon_inodes.c-		return ERR_PTR(error);
--
fs/f2fs/xattr.c-int f2fs_init_security(struct inode *inode, struct inode *dir,
fs/f2fs/xattr.c-				const struct qstr *qstr, struct page *ipage)
fs/f2fs/xattr.c-{
fs/f2fs/xattr.c:	return security_inode_init_security(inode, dir, qstr,
fs/f2fs/xattr.c-				&f2fs_initxattrs, ipage);
fs/f2fs/xattr.c-}
fs/f2fs/xattr.c-#endif
--
fs/jfs/xattr.c-int jfs_init_security(tid_t tid, struct inode *inode, struct inode *dir,
fs/jfs/xattr.c-		      const struct qstr *qstr)
fs/jfs/xattr.c-{
fs/jfs/xattr.c:	return security_inode_init_security(inode, dir, qstr,
fs/jfs/xattr.c-					    &jfs_initxattrs, &tid);
fs/jfs/xattr.c-}
fs/jfs/xattr.c-#endif
--
fs/reiserfs/xattr_security.c-	if (IS_PRIVATE(dir))
fs/reiserfs/xattr_security.c-		return 0;
fs/reiserfs/xattr_security.c-
fs/reiserfs/xattr_security.c:	error = security_inode_init_security(inode, dir, qstr,
fs/reiserfs/xattr_security.c-					     &reiserfs_initxattrs, sec);
fs/reiserfs/xattr_security.c-	if (error) {
fs/reiserfs/xattr_security.c-		sec->name = NULL;
--
fs/ext2/xattr_security.c-ext2_init_security(struct inode *inode, struct inode *dir,
fs/ext2/xattr_security.c-		   const struct qstr *qstr)
fs/ext2/xattr_security.c-{
fs/ext2/xattr_security.c:	return security_inode_init_security(inode, dir, qstr,
fs/ext2/xattr_security.c-					    &ext2_initxattrs, NULL);
fs/ext2/xattr_security.c-}
fs/ext2/xattr_security.c-
--
fs/ext4/xattr_security.c-ext4_init_security(handle_t *handle, struct inode *inode, struct inode *dir,
fs/ext4/xattr_security.c-		   const struct qstr *qstr)
fs/ext4/xattr_security.c-{
fs/ext4/xattr_security.c:	return security_inode_init_security(inode, dir, qstr,
fs/ext4/xattr_security.c-					    &ext4_initxattrs, handle);
fs/ext4/xattr_security.c-}
fs/ext4/xattr_security.c-
--
fs/ocfs2/xattr.c-	if (!ocfs2_supports_xattr(OCFS2_SB(dir->i_sb)))
fs/ocfs2/xattr.c-		return -EOPNOTSUPP;
fs/ocfs2/xattr.c-	if (si) {
fs/ocfs2/xattr.c:		ret = security_inode_init_security(inode, dir, qstr,
fs/ocfs2/xattr.c-						   &ocfs2_initxattrs, si);
fs/ocfs2/xattr.c-		/*
fs/ocfs2/xattr.c:		 * security_inode_init_security() does not return -EOPNOTSUPP,
fs/ocfs2/xattr.c-		 * we have to check the xattr ourselves.
fs/ocfs2/xattr.c-		 */
fs/ocfs2/xattr.c-		if (!ret && !si->name)
--
fs/ocfs2/xattr.c-		return ret;
fs/ocfs2/xattr.c-	}
fs/ocfs2/xattr.c-
fs/ocfs2/xattr.c:	return security_inode_init_security(inode, dir, qstr,
fs/ocfs2/xattr.c-					    &ocfs2_initxattrs, NULL);
fs/ocfs2/xattr.c-}
fs/ocfs2/xattr.c-
--
fs/hfsplus/xattr_security.c-int hfsplus_init_security(struct inode *inode, struct inode *dir,
fs/hfsplus/xattr_security.c-				const struct qstr *qstr)
fs/hfsplus/xattr_security.c-{
fs/hfsplus/xattr_security.c:	return security_inode_init_security(inode, dir, qstr,
fs/hfsplus/xattr_security.c-					&hfsplus_initxattrs, NULL);
fs/hfsplus/xattr_security.c-}
fs/hfsplus/xattr_security.c-
--
fs/btrfs/xattr.c-			      struct inode *inode, struct inode *dir,
fs/btrfs/xattr.c-			      const struct qstr *qstr)
fs/btrfs/xattr.c-{
fs/btrfs/xattr.c:	return security_inode_init_security(inode, dir, qstr,
fs/btrfs/xattr.c-					    &btrfs_initxattrs, trans);
fs/btrfs/xattr.c-}
--
fs/jffs2/security.c-int jffs2_init_security(struct inode *inode, struct inode *dir,
fs/jffs2/security.c-			const struct qstr *qstr)
fs/jffs2/security.c-{
fs/jffs2/security.c:	return security_inode_init_security(inode, dir, qstr,
fs/jffs2/security.c-					    &jffs2_initxattrs, NULL);
fs/jffs2/security.c-}
fs/jffs2/security.c-
```

---

# security hook: security_inode_init_security_anon

mm/secretmem.c:1
fs/anon_inodes.c:1

```shell
mm/secretmem.c-	 * bypassed for secretmem file descriptors.
mm/secretmem.c-	 */
mm/secretmem.c-	inode->i_flags &= ~S_PRIVATE;
mm/secretmem.c:	err = security_inode_init_security_anon(inode, &qname, NULL);
mm/secretmem.c-	if (err) {
mm/secretmem.c-		file = ERR_PTR(err);
mm/secretmem.c-		goto err_free_inode;
--
fs/anon_inodes.c-	if (IS_ERR(inode))
fs/anon_inodes.c-		return inode;
fs/anon_inodes.c-	inode->i_flags &= ~S_PRIVATE;
fs/anon_inodes.c:	error =	security_inode_init_security_anon(inode, &qname, context_inode);
fs/anon_inodes.c-	if (error) {
fs/anon_inodes.c-		iput(inode);
fs/anon_inodes.c-		return ERR_PTR(error);
```

---

# security hook: security_inode_invalidate_secctx

fs/ceph/super.h:1
fs/gfs2/glops.c:1

```shell
fs/ceph/super.h-				     struct ceph_acl_sec_ctx *ctx);
fs/ceph/super.h-static inline void ceph_security_invalidate_secctx(struct inode *inode)
fs/ceph/super.h-{
fs/ceph/super.h:	security_inode_invalidate_secctx(inode);
fs/ceph/super.h-}
fs/ceph/super.h-#else
fs/ceph/super.h-static inline int ceph_security_init_secctx(struct dentry *dentry, umode_t mode,
--
fs/gfs2/glops.c-		if (ip) {
fs/gfs2/glops.c-			set_bit(GLF_INSTANTIATE_NEEDED, &gl->gl_flags);
fs/gfs2/glops.c-			forget_all_cached_acls(&ip->i_inode);
fs/gfs2/glops.c:			security_inode_invalidate_secctx(&ip->i_inode);
fs/gfs2/glops.c-			gfs2_dir_hash_inval(ip);
fs/gfs2/glops.c-		}
fs/gfs2/glops.c-	}
```

---

# security hook: security_inode_killpriv

fs/attr.c:1

```shell
fs/attr.c-	if (ia_valid & ATTR_KILL_PRIV) {
fs/attr.c-		int error;
fs/attr.c-
fs/attr.c:		error = security_inode_killpriv(idmap, dentry);
fs/attr.c-		if (error)
fs/attr.c-			return error;
fs/attr.c-	}
```

---

# security hook: security_inode_link

fs/namei.c:1

```shell
fs/namei.c-	if (S_ISDIR(inode->i_mode))
fs/namei.c-		return -EPERM;
fs/namei.c-
fs/namei.c:	error = security_inode_link(old_dentry, dir, new_dentry);
fs/namei.c-	if (error)
fs/namei.c-		return error;
fs/namei.c-
```

---

# security hook: security_inode_listsecurity

net/socket.c:1
fs/nfs/nfs4proc.c:2
fs/xattr.c:2

```shell
net/socket.c-	ssize_t len;
net/socket.c-	ssize_t used = 0;
net/socket.c-
net/socket.c:	len = security_inode_listsecurity(d_inode(dentry), buffer, size);
net/socket.c-	if (len < 0)
net/socket.c-		return len;
net/socket.c-	used += len;
--
fs/nfs/nfs4proc.c-	int len = 0;
fs/nfs/nfs4proc.c-
fs/nfs/nfs4proc.c-	if (nfs_server_capable(inode, NFS_CAP_SECURITY_LABEL)) {
fs/nfs/nfs4proc.c:		len = security_inode_listsecurity(inode, list, list_len);
fs/nfs/nfs4proc.c-		if (len >= 0 && list_len && len > list_len)
fs/nfs/nfs4proc.c-			return -ERANGE;
fs/nfs/nfs4proc.c-	}
--
fs/nfs/nfs4proc.c-	}
fs/nfs/nfs4proc.c-
fs/nfs/nfs4proc.c-	if (!nfs_server_capable(d_inode(dentry), NFS_CAP_SECURITY_LABEL)) {
fs/nfs/nfs4proc.c:		error4 = security_inode_listsecurity(d_inode(dentry), list, left);
fs/nfs/nfs4proc.c-		if (error4 < 0)
fs/nfs/nfs4proc.c-			return error4;
fs/nfs/nfs4proc.c-	}
--
fs/xattr.c-	if (inode->i_op->listxattr) {
fs/xattr.c-		error = inode->i_op->listxattr(dentry, list, size);
fs/xattr.c-	} else {
fs/xattr.c:		error = security_inode_listsecurity(inode, list, size);
fs/xattr.c-		if (size && error > size)
fs/xattr.c-			error = -ERANGE;
fs/xattr.c-	}
--
fs/xattr.c-	if (err)
fs/xattr.c-		return err;
fs/xattr.c-
fs/xattr.c:	err = security_inode_listsecurity(inode, buffer, remaining_size);
fs/xattr.c-	if (err < 0)
fs/xattr.c-		return err;
fs/xattr.c-
```

---

# security hook: security_inode_listxattr

fs/xattr.c:1

```shell
fs/xattr.c-	struct inode *inode = d_inode(dentry);
fs/xattr.c-	ssize_t error;
fs/xattr.c-
fs/xattr.c:	error = security_inode_listxattr(dentry);
fs/xattr.c-	if (error)
fs/xattr.c-		return error;
fs/xattr.c-
```

---

# security hook: security_inode_mkdir

fs/cachefiles/security.c:1
fs/namei.c:1

```shell
fs/cachefiles/security.c-{
fs/cachefiles/security.c-	int ret;
fs/cachefiles/security.c-
fs/cachefiles/security.c:	ret = security_inode_mkdir(d_backing_inode(root), root, 0);
fs/cachefiles/security.c-	if (ret < 0) {
fs/cachefiles/security.c-		pr_err("Security denies permission to make dirs: error %d",
fs/cachefiles/security.c-		       ret);
--
fs/namei.c-		return -EPERM;
fs/namei.c-
fs/namei.c-	mode = vfs_prepare_mode(idmap, dir, mode, S_IRWXUGO | S_ISVTX, 0);
fs/namei.c:	error = security_inode_mkdir(dir, dentry, mode);
fs/namei.c-	if (error)
fs/namei.c-		return error;
fs/namei.c-
```

---

# security hook: security_inode_mknod

fs/namei.c:1

```shell
fs/namei.c-	if (error)
fs/namei.c-		return error;
fs/namei.c-
fs/namei.c:	error = security_inode_mknod(dir, dentry, mode, dev);
fs/namei.c-	if (error)
fs/namei.c-		return error;
fs/namei.c-
```

---

# security hook: security_inode_need_killpriv

fs/inode.c:1
fs/attr.c:1

```shell
fs/inode.c-		return 0;
fs/inode.c-
fs/inode.c-	mask = setattr_should_drop_suidgid(idmap, inode);
fs/inode.c:	ret = security_inode_need_killpriv(dentry);
fs/inode.c-	if (ret < 0)
fs/inode.c-		return ret;
fs/inode.c-	if (ret)
--
fs/attr.c-		attr->ia_mtime = timestamp_truncate(attr->ia_mtime, inode);
fs/attr.c-
fs/attr.c-	if (ia_valid & ATTR_KILL_PRIV) {
fs/attr.c:		error = security_inode_need_killpriv(dentry);
fs/attr.c-		if (error < 0)
fs/attr.c-			return error;
fs/attr.c-		if (error == 0)
```

---

# security hook: security_inode_notifysecctx

fs/nfs/inode.c:2

```shell
fs/nfs/inode.c-		return;
fs/nfs/inode.c-
fs/nfs/inode.c-	if ((fattr->valid & NFS_ATTR_FATTR_V4_SECURITY_LABEL) && inode->i_security) {
fs/nfs/inode.c:		error = security_inode_notifysecctx(inode, fattr->label->label,
fs/nfs/inode.c-				fattr->label->len);
fs/nfs/inode.c-		if (error)
fs/nfs/inode.c-			printk(KERN_ERR "%s() %s %d "
fs/nfs/inode.c:					"security_inode_notifysecctx() %d\n",
fs/nfs/inode.c-					__func__,
fs/nfs/inode.c-					(char *)fattr->label->label,
fs/nfs/inode.c-					fattr->label->len, error);
```

---

# security hook: security_inode_permission

Documentation/security/lsm.rst:1
fs/namei.c:1

```shell
Documentation/security/lsm.rst-These hooks are used to allocate
Documentation/security/lsm.rst-and free security structures for inode objects.
Documentation/security/lsm.rst-An example of the second category of hooks
Documentation/security/lsm.rst:is the security_inode_permission() hook.
Documentation/security/lsm.rst-This hook checks permission when accessing an inode.
Documentation/security/lsm.rst-
Documentation/security/lsm.rst-LSM Capabilities Module
--
fs/namei.c-	if (retval)
fs/namei.c-		return retval;
fs/namei.c-
fs/namei.c:	return security_inode_permission(inode, mask);
fs/namei.c-}
fs/namei.c-EXPORT_SYMBOL(inode_permission);
fs/namei.c-
```

---

# security hook: security_inode_post_setxattr

fs/xattr.c:1

```shell
fs/xattr.c-				       size, flags);
fs/xattr.c-		if (!error) {
fs/xattr.c-			fsnotify_xattr(dentry);
fs/xattr.c:			security_inode_post_setxattr(dentry, name, value,
fs/xattr.c-						     size, flags);
fs/xattr.c-		}
fs/xattr.c-	} else {
```

---

# security hook: security_inode_readlink

fs/namei.c:1
fs/stat.c:1
fs/overlayfs/inode.c:1

```shell
fs/overlayfs/inode.c- * > This is a similar situation as reading a symlink vs. following it.
fs/overlayfs/inode.c- * > When following a symlink overlayfs always reads the link on the
fs/overlayfs/inode.c- * > underlying fs just as if it was a readlink(2) call, calling
fs/overlayfs/inode.c: * > security_inode_readlink() instead of security_inode_follow_link().
fs/overlayfs/inode.c- * > This is logical: we are reading the link from the underlying storage,
fs/overlayfs/inode.c- * > and following it on overlayfs.
fs/overlayfs/inode.c- * >
--
fs/stat.c-		 * AFS mountpoints allow readlink(2) but are not symlinks
fs/stat.c-		 */
fs/stat.c-		if (d_is_symlink(path.dentry) || inode->i_op->readlink) {
fs/stat.c:			error = security_inode_readlink(path.dentry);
fs/stat.c-			if (!error) {
fs/stat.c-				touch_atime(&path);
fs/stat.c-				error = vfs_readlink(path.dentry, buf, bufsiz);
--
fs/namei.c-	struct inode *inode = d_inode(dentry);
fs/namei.c-
fs/namei.c-	if (d_is_symlink(dentry)) {
fs/namei.c:		res = ERR_PTR(security_inode_readlink(dentry));
fs/namei.c-		if (!res)
fs/namei.c-			res = inode->i_op->get_link(dentry, inode, done);
fs/namei.c-	}
```

---

# security hook: security_inode_remove_acl

fs/posix_acl.c:1

```shell
fs/posix_acl.c-	if (error)
fs/posix_acl.c-		goto out_inode_unlock;
fs/posix_acl.c-
fs/posix_acl.c:	error = security_inode_remove_acl(idmap, dentry, acl_name);
fs/posix_acl.c-	if (error)
fs/posix_acl.c-		goto out_inode_unlock;
fs/posix_acl.c-
```

---

# security hook: security_inode_removexattr

fs/xattr.c:1

```shell
fs/xattr.c-	if (error)
fs/xattr.c-		return error;
fs/xattr.c-
fs/xattr.c:	error = security_inode_removexattr(idmap, dentry, name);
fs/xattr.c-	if (error)
fs/xattr.c-		goto out;
fs/xattr.c-
```

---

# security hook: security_inode_rename

fs/namei.c:1

```shell
fs/namei.c-		}
fs/namei.c-	}
fs/namei.c-
fs/namei.c:	error = security_inode_rename(old_dir, old_dentry, new_dir, new_dentry,
fs/namei.c-				      flags);
fs/namei.c-	if (error)
fs/namei.c-		return error;
```

---

# security hook: security_inode_rmdir

fs/namei.c:1

```shell
fs/namei.c-	    (dentry->d_inode->i_flags & S_KERNEL_FILE))
fs/namei.c-		goto out;
fs/namei.c-
fs/namei.c:	error = security_inode_rmdir(dir, dentry);
fs/namei.c-	if (error)
fs/namei.c-		goto out;
fs/namei.c-
```

---

# security hook: security_inode_set_acl

fs/posix_acl.c:1

```shell
fs/posix_acl.c-	if (error)
fs/posix_acl.c-		goto out_inode_unlock;
fs/posix_acl.c-
fs/posix_acl.c:	error = security_inode_set_acl(idmap, dentry, acl_name, kacl);
fs/posix_acl.c-	if (error)
fs/posix_acl.c-		goto out_inode_unlock;
fs/posix_acl.c-
```

---

# security hook: security_inode_setattr

fs/fat/file.c:1
fs/attr.c:1

```shell
fs/fat/file.c-	 * out the RO attribute for checking by the security
fs/fat/file.c-	 * module, just because it maps to a file mode.
fs/fat/file.c-	 */
fs/fat/file.c:	err = security_inode_setattr(file_mnt_idmap(file),
fs/fat/file.c-				     file->f_path.dentry, &ia);
fs/fat/file.c-	if (err)
fs/fat/file.c-		goto out_unlock_inode;
--
fs/attr.c-	    !vfsgid_valid(i_gid_into_vfsgid(idmap, inode)))
fs/attr.c-		return -EOVERFLOW;
fs/attr.c-
fs/attr.c:	error = security_inode_setattr(idmap, dentry, attr);
fs/attr.c-	if (error)
fs/attr.c-		return error;
fs/attr.c-	error = try_break_deleg(inode, delegated_inode);
```

---

# security hook: security_inode_setsecctx

fs/nfsd/vfs.c:1

```shell
fs/nfsd/vfs.c-			break;
fs/nfsd/vfs.c-	}
fs/nfsd/vfs.c-	if (attr->na_seclabel && attr->na_seclabel->len)
fs/nfsd/vfs.c:		attr->na_labelerr = security_inode_setsecctx(dentry,
fs/nfsd/vfs.c-			attr->na_seclabel->data, attr->na_seclabel->len);
fs/nfsd/vfs.c-	if (IS_ENABLED(CONFIG_FS_POSIX_ACL) && attr->na_pacl)
fs/nfsd/vfs.c-		attr->na_aclerr = set_posix_acl(&nop_mnt_idmap,
```

---

# security hook: security_inode_setsecurity

fs/xattr.c:1

```shell
fs/xattr.c-		if (issec) {
fs/xattr.c-			const char *suffix = name + XATTR_SECURITY_PREFIX_LEN;
fs/xattr.c-
fs/xattr.c:			error = security_inode_setsecurity(inode, suffix, value,
fs/xattr.c-							   size, flags);
fs/xattr.c-			if (!error)
fs/xattr.c-				fsnotify_xattr(dentry);
```

---

# security hook: security_inode_setxattr

fs/xattr.c:1

```shell
fs/xattr.c-	if (error)
fs/xattr.c-		return error;
fs/xattr.c-
fs/xattr.c:	error = security_inode_setxattr(idmap, dentry, name, value, size,
fs/xattr.c-					flags);
fs/xattr.c-	if (error)
fs/xattr.c-		goto out;
```

---

# security hook: security_inode_symlink

fs/namei.c:1

```shell
fs/namei.c-	if (!dir->i_op->symlink)
fs/namei.c-		return -EPERM;
fs/namei.c-
fs/namei.c:	error = security_inode_symlink(dir, dentry, oldname);
fs/namei.c-	if (error)
fs/namei.c-		return error;
fs/namei.c-
```

---

# security hook: security_inode_unlink

fs/namei.c:1

```shell
fs/namei.c-	else if (is_local_mountpoint(dentry))
fs/namei.c-		error = -EBUSY;
fs/namei.c-	else {
fs/namei.c:		error = security_inode_unlink(dir, dentry);
fs/namei.c-		if (!error) {
fs/namei.c-			error = try_break_deleg(target, delegated_inode);
fs/namei.c-			if (error)
```

---

# security hook: security_ipc_getsecid

kernel/auditsc.c:1

```shell
kernel/auditsc.c-	context->ipc.gid = ipcp->gid;
kernel/auditsc.c-	context->ipc.mode = ipcp->mode;
kernel/auditsc.c-	context->ipc.has_perm = 0;
kernel/auditsc.c:	security_ipc_getsecid(ipcp, &context->ipc.osid);
kernel/auditsc.c-	context->type = AUDIT_IPC;
kernel/auditsc.c-}
kernel/auditsc.c-
```

---

# security hook: security_ipc_permission

ipc/util.c:1

```shell
ipc/util.c-	    !ns_capable(ns->user_ns, CAP_IPC_OWNER))
ipc/util.c-		return -1;
ipc/util.c-
ipc/util.c:	return security_ipc_permission(ipcp, flag);
ipc/util.c-}
ipc/util.c-
ipc/util.c-/*
```

---

# security hook: security_ismaclabel

fs/ceph/xattr.c:1
fs/nfs/nfs4proc.c:2
fs/xattr.c:1

```shell
fs/ceph/xattr.c-
fs/ceph/xattr.c-	if (current->journal_info &&
fs/ceph/xattr.c-	    !strncmp(name, XATTR_SECURITY_PREFIX, XATTR_SECURITY_PREFIX_LEN) &&
fs/ceph/xattr.c:	    security_ismaclabel(name + XATTR_SECURITY_PREFIX_LEN))
fs/ceph/xattr.c-		ci->i_ceph_flags |= CEPH_I_SEC_INITED;
fs/ceph/xattr.c-out:
fs/ceph/xattr.c-	spin_unlock(&ci->i_ceph_lock);
--
fs/nfs/nfs4proc.c-				     const char *key, const void *buf,
fs/nfs/nfs4proc.c-				     size_t buflen, int flags)
fs/nfs/nfs4proc.c-{
fs/nfs/nfs4proc.c:	if (security_ismaclabel(key))
fs/nfs/nfs4proc.c-		return nfs4_set_security_label(inode, buf, buflen);
fs/nfs/nfs4proc.c-
fs/nfs/nfs4proc.c-	return -EOPNOTSUPP;
--
fs/nfs/nfs4proc.c-				     struct dentry *unused, struct inode *inode,
fs/nfs/nfs4proc.c-				     const char *key, void *buf, size_t buflen)
fs/nfs/nfs4proc.c-{
fs/nfs/nfs4proc.c:	if (security_ismaclabel(key))
fs/nfs/nfs4proc.c-		return nfs4_get_security_label(inode, buf, buflen);
fs/nfs/nfs4proc.c-	return -EOPNOTSUPP;
fs/nfs/nfs4proc.c-}
--
fs/xattr.c-
fs/xattr.c-	return !strncmp(name, XATTR_SECURITY_PREFIX,
fs/xattr.c-			XATTR_SECURITY_PREFIX_LEN) &&
fs/xattr.c:		security_ismaclabel(suffix);
fs/xattr.c-}
fs/xattr.c-
fs/xattr.c-/**
```

---

# security hook: security_kernel_act_as

kernel/cred.c:1

```shell
kernel/cred.c- */
kernel/cred.c-int set_security_override(struct cred *new, u32 secid)
kernel/cred.c-{
kernel/cred.c:	return security_kernel_act_as(new, secid);
kernel/cred.c-}
kernel/cred.c-EXPORT_SYMBOL(set_security_override);
kernel/cred.c-
```

---

# security hook: security_kernel_create_files_as

kernel/cred.c:1

```shell
kernel/cred.c-		return -EINVAL;
kernel/cred.c-	new->fsuid = inode->i_uid;
kernel/cred.c-	new->fsgid = inode->i_gid;
kernel/cred.c:	return security_kernel_create_files_as(new, inode);
kernel/cred.c-}
kernel/cred.c-EXPORT_SYMBOL(set_create_files_as);
```

---

# security hook: security_kernel_load_data

kernel/kexec.c:1
kernel/module/main.c:1
drivers/base/firmware_loader/fallback.c:1
drivers/base/firmware_loader/fallback_platform.c:1

```shell
kernel/kexec.c-		return -EPERM;
kernel/kexec.c-
kernel/kexec.c-	/* Permit LSMs and IMA to fail the kexec */
kernel/kexec.c:	result = security_kernel_load_data(LOADING_KEXEC_IMAGE, false);
kernel/kexec.c-	if (result < 0)
kernel/kexec.c-		return result;
kernel/kexec.c-
--
kernel/module/main.c-	if (info->len < sizeof(*(info->hdr)))
kernel/module/main.c-		return -ENOEXEC;
kernel/module/main.c-
kernel/module/main.c:	err = security_kernel_load_data(LOADING_MODULE, true);
kernel/module/main.c-	if (err)
kernel/module/main.c-		return err;
kernel/module/main.c-
--
drivers/base/firmware_loader/fallback.c-		return false;
drivers/base/firmware_loader/fallback.c-
drivers/base/firmware_loader/fallback.c-	/* Also permit LSMs and IMA to fail firmware sysfs fallback */
drivers/base/firmware_loader/fallback.c:	ret = security_kernel_load_data(LOADING_FIRMWARE, true);
drivers/base/firmware_loader/fallback.c-	if (ret < 0)
drivers/base/firmware_loader/fallback.c-		return false;
drivers/base/firmware_loader/fallback.c-
--
drivers/base/firmware_loader/fallback_platform.c-	if (!(fw_priv->opt_flags & FW_OPT_FALLBACK_PLATFORM))
drivers/base/firmware_loader/fallback_platform.c-		return -ENOENT;
drivers/base/firmware_loader/fallback_platform.c-
drivers/base/firmware_loader/fallback_platform.c:	rc = security_kernel_load_data(LOADING_FIRMWARE, true);
drivers/base/firmware_loader/fallback_platform.c-	if (rc)
drivers/base/firmware_loader/fallback_platform.c-		return rc;
drivers/base/firmware_loader/fallback_platform.c-
```

---

# security hook: security_kernel_module_request

kernel/module/kmod.c:1

```shell
kernel/module/kmod.c-	if (ret >= MODULE_NAME_LEN)
kernel/module/kmod.c-		return -ENAMETOOLONG;
kernel/module/kmod.c-
kernel/module/kmod.c:	ret = security_kernel_module_request(module_name);
kernel/module/kmod.c-	if (ret)
kernel/module/kmod.c-		return ret;
kernel/module/kmod.c-
```

---

# security hook: security_kernel_post_load_data

kernel/module/main.c:1
drivers/base/firmware_loader/sysfs.c:1
drivers/base/firmware_loader/fallback_platform.c:1

```shell
kernel/module/main.c-		goto out;
kernel/module/main.c-	}
kernel/module/main.c-
kernel/module/main.c:	err = security_kernel_post_load_data((char *)info->hdr, info->len,
kernel/module/main.c-					     LOADING_MODULE, "init_module");
kernel/module/main.c-out:
kernel/module/main.c-	if (err)
--
drivers/base/firmware_loader/sysfs.c-				dev_err(dev, "%s: map pages failed\n",
drivers/base/firmware_loader/sysfs.c-					__func__);
drivers/base/firmware_loader/sysfs.c-			else
drivers/base/firmware_loader/sysfs.c:				rc = security_kernel_post_load_data(fw_priv->data,
drivers/base/firmware_loader/sysfs.c-								    fw_priv->size,
drivers/base/firmware_loader/sysfs.c-								    LOADING_FIRMWARE,
drivers/base/firmware_loader/sysfs.c-								    "blob");
--
drivers/base/firmware_loader/fallback_platform.c-	if (fw_priv->data && size > fw_priv->allocated_size)
drivers/base/firmware_loader/fallback_platform.c-		return -ENOMEM;
drivers/base/firmware_loader/fallback_platform.c-
drivers/base/firmware_loader/fallback_platform.c:	rc = security_kernel_post_load_data((u8 *)data, size, LOADING_FIRMWARE,
drivers/base/firmware_loader/fallback_platform.c-						"platform");
drivers/base/firmware_loader/fallback_platform.c-	if (rc)
drivers/base/firmware_loader/fallback_platform.c-		return rc;
```

---

# security hook: security_kernel_post_read_file

fs/kernel_read_file.c:1

```shell
fs/kernel_read_file.c-			goto out_free;
fs/kernel_read_file.c-		}
fs/kernel_read_file.c-
fs/kernel_read_file.c:		ret = security_kernel_post_read_file(file, *buf, i_size, id);
fs/kernel_read_file.c-	}
fs/kernel_read_file.c-
fs/kernel_read_file.c-out_free:
```

---

# security hook: security_kernel_read_file

fs/kernel_read_file.c:1

```shell
fs/kernel_read_file.c-	}
fs/kernel_read_file.c-
fs/kernel_read_file.c-	whole_file = (offset == 0 && i_size <= buf_size);
fs/kernel_read_file.c:	ret = security_kernel_read_file(file, id, whole_file);
fs/kernel_read_file.c-	if (ret)
fs/kernel_read_file.c-		goto out;
fs/kernel_read_file.c-
```

---

# security hook: security_kernfs_init_security

fs/kernfs/dir.c:1

```shell
fs/kernfs/dir.c-	}
fs/kernfs/dir.c-
fs/kernfs/dir.c-	if (parent) {
fs/kernfs/dir.c:		ret = security_kernfs_init_security(parent, kn);
fs/kernfs/dir.c-		if (ret)
fs/kernfs/dir.c-			goto err_out3;
fs/kernfs/dir.c-	}
```

---

# security hook: security_key_alloc


```shell
```

---

# security hook: security_key_free


```shell
```

---

# security hook: security_key_getsecurity


```shell
```

---

# security hook: security_key_permission


```shell
```

---

# security hook: security_locked_down

kernel/power/hibernate.c:1
net/xfrm/xfrm_user.c:1
kernel/module/signing.c:1
kernel/trace/trace_events.c:1
kernel/params.c:1
kernel/trace/trace_events_trigger.c:1
kernel/debug/kdb/kdb_main.c:3
kernel/debug/debug_core.c:1
kernel/kexec_file.c:1
kernel/trace/ftrace.c:3
kernel/trace/trace_kprobe.c:3
kernel/trace/trace.c:5
kernel/trace/trace_stat.c:1
kernel/trace/ring_buffer.c:1
kernel/trace/bpf_trace.c:5
kernel/trace/trace_dynevent.c:1
kernel/trace/trace_printk.c:1
kernel/trace/trace_fprobe.c:1
kernel/trace/trace_uprobe.c:2
kernel/trace/trace_stack.c:1
kernel/trace/trace_events_synth.c:1
kernel/events/core.c:1
kernel/kexec.c:1
kernel/bpf/helpers.c:2
drivers/pci/syscall.c:1
drivers/pci/proc.c:3
drivers/pci/pci-sysfs.c:3
arch/s390/hypfs/hypfs_dbfs.c:1
drivers/firmware/efi/efi.c:1
drivers/firmware/efi/test/efi_test.c:1
arch/powerpc/xmon/xmon.c:2
fs/debugfs/inode.c:1
fs/debugfs/file.c:1
arch/x86/mm/testmmiotrace.c:1
fs/tracefs/inode.c:2
fs/tracefs/event_inode.c:1
fs/proc/kcore.c:1
arch/powerpc/platforms/pseries/reconfig.c:1
drivers/acpi/custom_method.c:1
drivers/platform/x86/intel/tpmi.c:1
drivers/pcmcia/cistpl.c:1
drivers/acpi/tables.c:1
drivers/acpi/osl.c:1
arch/powerpc/kernel/rtas.c:3
drivers/acpi/acpi_configfs.c:1
arch/x86/kernel/ioport.c:2
arch/x86/kernel/msr.c:2
drivers/char/mem.c:1
drivers/cxl/core/mbox.c:1
drivers/tty/serial/serial_core.c:1

```shell
net/xfrm/xfrm_user.c-static bool xfrm_redact(void)
net/xfrm/xfrm_user.c-{
net/xfrm/xfrm_user.c-	return IS_ENABLED(CONFIG_SECURITY) &&
net/xfrm/xfrm_user.c:		security_locked_down(LOCKDOWN_XFRM_SECRET);
net/xfrm/xfrm_user.c-}
net/xfrm/xfrm_user.c-
net/xfrm/xfrm_user.c-static int copy_to_user_auth(struct xfrm_algo_auth *auth, struct sk_buff *skb)
--
kernel/power/hibernate.c-bool hibernation_available(void)
kernel/power/hibernate.c-{
kernel/power/hibernate.c-	return nohibernate == 0 &&
kernel/power/hibernate.c:		!security_locked_down(LOCKDOWN_HIBERNATION) &&
kernel/power/hibernate.c-		!secretmem_active() && !cxl_mem_active();
kernel/power/hibernate.c-}
kernel/power/hibernate.c-
--
kernel/module/signing.c-		return -EKEYREJECTED;
kernel/module/signing.c-	}
kernel/module/signing.c-
kernel/module/signing.c:	return security_locked_down(LOCKDOWN_MODULE_SIGNATURE);
kernel/module/signing.c-}
--
kernel/trace/trace_events.c-	struct seq_file *m;
kernel/trace/trace_events.c-	int ret;
kernel/trace/trace_events.c-
kernel/trace/trace_events.c:	ret = security_locked_down(LOCKDOWN_TRACEFS);
kernel/trace/trace_events.c-	if (ret)
kernel/trace/trace_events.c-		return ret;
kernel/trace/trace_events.c-
--
kernel/trace/trace_events_trigger.c-{
kernel/trace/trace_events_trigger.c-	int ret;
kernel/trace/trace_events_trigger.c-
kernel/trace/trace_events_trigger.c:	ret = security_locked_down(LOCKDOWN_TRACEFS);
kernel/trace/trace_events_trigger.c-	if (ret)
kernel/trace/trace_events_trigger.c-		return ret;
kernel/trace/trace_events_trigger.c-
--
kernel/params.c-static bool param_check_unsafe(const struct kernel_param *kp)
kernel/params.c-{
kernel/params.c-	if (kp->flags & KERNEL_PARAM_FL_HWPARAM &&
kernel/params.c:	    security_locked_down(LOCKDOWN_MODULE_PARAMETERS))
kernel/params.c-		return false;
kernel/params.c-
kernel/params.c-	if (kp->flags & KERNEL_PARAM_FL_UNSAFE) {
--
kernel/trace/ftrace.c-	struct ftrace_iterator *iter;
kernel/trace/ftrace.c-	int ret;
kernel/trace/ftrace.c-
kernel/trace/ftrace.c:	ret = security_locked_down(LOCKDOWN_TRACEFS);
kernel/trace/ftrace.c-	if (ret)
kernel/trace/ftrace.c-		return ret;
kernel/trace/ftrace.c-
--
kernel/trace/ftrace.c-	struct ftrace_iterator *iter;
kernel/trace/ftrace.c-	int ret;
kernel/trace/ftrace.c-
kernel/trace/ftrace.c:	ret = security_locked_down(LOCKDOWN_TRACEFS);
kernel/trace/ftrace.c-	if (ret)
kernel/trace/ftrace.c-		return ret;
kernel/trace/ftrace.c-
--
kernel/trace/ftrace.c-	int ret;
kernel/trace/ftrace.c-	struct ftrace_hash *new_hash = NULL;
kernel/trace/ftrace.c-
kernel/trace/ftrace.c:	ret = security_locked_down(LOCKDOWN_TRACEFS);
kernel/trace/ftrace.c-	if (ret)
kernel/trace/ftrace.c-		return ret;
kernel/trace/ftrace.c-
--
kernel/trace/trace_kprobe.c-{
kernel/trace/trace_kprobe.c-	int i, ret;
kernel/trace/trace_kprobe.c-
kernel/trace/trace_kprobe.c:	ret = security_locked_down(LOCKDOWN_KPROBES);
kernel/trace/trace_kprobe.c-	if (ret)
kernel/trace/trace_kprobe.c-		return ret;
kernel/trace/trace_kprobe.c-
--
kernel/trace/trace_kprobe.c-{
kernel/trace/trace_kprobe.c-	int ret;
kernel/trace/trace_kprobe.c-
kernel/trace/trace_kprobe.c:	ret = security_locked_down(LOCKDOWN_TRACEFS);
kernel/trace/trace_kprobe.c-	if (ret)
kernel/trace/trace_kprobe.c-		return ret;
kernel/trace/trace_kprobe.c-
--
kernel/trace/trace_kprobe.c-{
kernel/trace/trace_kprobe.c-	int ret;
kernel/trace/trace_kprobe.c-
kernel/trace/trace_kprobe.c:	ret = security_locked_down(LOCKDOWN_TRACEFS);
kernel/trace/trace_kprobe.c-	if (ret)
kernel/trace/trace_kprobe.c-		return ret;
kernel/trace/trace_kprobe.c-
--
kernel/debug/kdb/kdb_main.c- * Update the permissions flags (kdb_cmd_enabled) to match the
kernel/debug/kdb/kdb_main.c- * current lockdown state.
kernel/debug/kdb/kdb_main.c- *
kernel/debug/kdb/kdb_main.c: * Within this function the calls to security_locked_down() are "lazy". We
kernel/debug/kdb/kdb_main.c- * avoid calling them if the current value of kdb_cmd_enabled already excludes
kernel/debug/kdb/kdb_main.c- * flags that might be subject to lockdown. Additionally we deliberately check
kernel/debug/kdb/kdb_main.c- * the lockdown flags independently (even though read lockdown implies write
--
kernel/debug/kdb/kdb_main.c-
kernel/debug/kdb/kdb_main.c-	if (kdb_cmd_enabled & (KDB_ENABLE_ALL | write_flags))
kernel/debug/kdb/kdb_main.c-		need_to_lockdown_write =
kernel/debug/kdb/kdb_main.c:			security_locked_down(LOCKDOWN_DBG_WRITE_KERNEL);
kernel/debug/kdb/kdb_main.c-
kernel/debug/kdb/kdb_main.c-	if (kdb_cmd_enabled & (KDB_ENABLE_ALL | read_flags))
kernel/debug/kdb/kdb_main.c-		need_to_lockdown_read =
kernel/debug/kdb/kdb_main.c:			security_locked_down(LOCKDOWN_DBG_READ_KERNEL);
kernel/debug/kdb/kdb_main.c-
kernel/debug/kdb/kdb_main.c-	/* De-compose KDB_ENABLE_ALL if required */
kernel/debug/kdb/kdb_main.c-	if (need_to_lockdown_write || need_to_lockdown_read)
--
kernel/debug/debug_core.c-			 * themselves, especially with help from the lockdown
kernel/debug/debug_core.c-			 * message printed on the console!
kernel/debug/debug_core.c-			 */
kernel/debug/debug_core.c:			if (security_locked_down(LOCKDOWN_DBG_WRITE_KERNEL)) {
kernel/debug/debug_core.c-				if (IS_ENABLED(CONFIG_KGDB_KDB)) {
kernel/debug/debug_core.c-					/* Switch back to kdb if possible... */
kernel/debug/debug_core.c-					dbg_kdb_mode = 1;
--
kernel/kexec_file.c-		 * down.
kernel/kexec_file.c-		 */
kernel/kexec_file.c-		if (!ima_appraise_signature(READING_KEXEC_IMAGE) &&
kernel/kexec_file.c:		    security_locked_down(LOCKDOWN_KEXEC))
kernel/kexec_file.c-			return -EPERM;
kernel/kexec_file.c-
kernel/kexec_file.c-		pr_debug("kernel signature verification failed (%d).\n", ret);
--
kernel/trace/trace.c-{
kernel/trace/trace.c-	int ret;
kernel/trace/trace.c-
kernel/trace/trace.c:	ret = security_locked_down(LOCKDOWN_TRACEFS);
kernel/trace/trace.c-	if (ret)
kernel/trace/trace.c-		return ret;
kernel/trace/trace.c-
--
kernel/trace/trace.c-		return -1;
kernel/trace/trace.c-	}
kernel/trace/trace.c-
kernel/trace/trace.c:	if (security_locked_down(LOCKDOWN_TRACEFS)) {
kernel/trace/trace.c-		pr_warn("Can not register tracer %s due to lockdown\n",
kernel/trace/trace.c-			   type->name);
kernel/trace/trace.c-		return -EPERM;
--
kernel/trace/trace.c-{
kernel/trace/trace.c-	struct trace_array *tr = &global_trace;
kernel/trace/trace.c-
kernel/trace/trace.c:	if (security_locked_down(LOCKDOWN_TRACEFS)) {
kernel/trace/trace.c-		pr_warn("Tracing disabled due to lockdown\n");
kernel/trace/trace.c-		return -EPERM;
kernel/trace/trace.c-	}
--
kernel/trace/trace.c-	int ret = -ENOMEM;
kernel/trace/trace.c-
kernel/trace/trace.c-
kernel/trace/trace.c:	if (security_locked_down(LOCKDOWN_TRACEFS)) {
kernel/trace/trace.c-		pr_warn("Tracing disabled due to lockdown\n");
kernel/trace/trace.c-		return -EPERM;
kernel/trace/trace.c-	}
--
kernel/trace/trace.c-{
kernel/trace/trace.c-	/* sched_clock_stable() is determined in late_initcall */
kernel/trace/trace.c-	if (!trace_boot_clock && !sched_clock_stable()) {
kernel/trace/trace.c:		if (security_locked_down(LOCKDOWN_TRACEFS)) {
kernel/trace/trace.c-			pr_warn("Can not set tracing clock due to lockdown\n");
kernel/trace/trace.c-			return;
kernel/trace/trace.c-		}
--
kernel/trace/trace_stat.c-	struct seq_file *m;
kernel/trace/trace_stat.c-	struct stat_session *session = inode->i_private;
kernel/trace/trace_stat.c-
kernel/trace/trace_stat.c:	ret = security_locked_down(LOCKDOWN_TRACEFS);
kernel/trace/trace_stat.c-	if (ret)
kernel/trace/trace_stat.c-		return ret;
kernel/trace/trace_stat.c-
--
kernel/trace/ring_buffer.c-	int cpu;
kernel/trace/ring_buffer.c-	int ret = 0;
kernel/trace/ring_buffer.c-
kernel/trace/ring_buffer.c:	if (security_locked_down(LOCKDOWN_TRACEFS)) {
kernel/trace/ring_buffer.c-		pr_warn("Lockdown is enabled, skipping ring buffer tests\n");
kernel/trace/ring_buffer.c-		return 0;
kernel/trace/ring_buffer.c-	}
--
kernel/trace/bpf_trace.c-	case BPF_FUNC_get_prandom_u32:
kernel/trace/bpf_trace.c-		return &bpf_get_prandom_u32_proto;
kernel/trace/bpf_trace.c-	case BPF_FUNC_probe_write_user:
kernel/trace/bpf_trace.c:		return security_locked_down(LOCKDOWN_BPF_WRITE_USER) < 0 ?
kernel/trace/bpf_trace.c-		       NULL : bpf_get_probe_write_proto();
kernel/trace/bpf_trace.c-	case BPF_FUNC_probe_read_user:
kernel/trace/bpf_trace.c-		return &bpf_probe_read_user_proto;
kernel/trace/bpf_trace.c-	case BPF_FUNC_probe_read_kernel:
kernel/trace/bpf_trace.c:		return security_locked_down(LOCKDOWN_BPF_READ_KERNEL) < 0 ?
kernel/trace/bpf_trace.c-		       NULL : &bpf_probe_read_kernel_proto;
kernel/trace/bpf_trace.c-	case BPF_FUNC_probe_read_user_str:
kernel/trace/bpf_trace.c-		return &bpf_probe_read_user_str_proto;
kernel/trace/bpf_trace.c-	case BPF_FUNC_probe_read_kernel_str:
kernel/trace/bpf_trace.c:		return security_locked_down(LOCKDOWN_BPF_READ_KERNEL) < 0 ?
kernel/trace/bpf_trace.c-		       NULL : &bpf_probe_read_kernel_str_proto;
kernel/trace/bpf_trace.c-#ifdef CONFIG_ARCH_HAS_NON_OVERLAPPING_ADDRESS_SPACE
kernel/trace/bpf_trace.c-	case BPF_FUNC_probe_read:
kernel/trace/bpf_trace.c:		return security_locked_down(LOCKDOWN_BPF_READ_KERNEL) < 0 ?
kernel/trace/bpf_trace.c-		       NULL : &bpf_probe_read_compat_proto;
kernel/trace/bpf_trace.c-	case BPF_FUNC_probe_read_str:
kernel/trace/bpf_trace.c:		return security_locked_down(LOCKDOWN_BPF_READ_KERNEL) < 0 ?
kernel/trace/bpf_trace.c-		       NULL : &bpf_probe_read_compat_str_proto;
kernel/trace/bpf_trace.c-#endif
kernel/trace/bpf_trace.c-#ifdef CONFIG_CGROUPS
--
kernel/trace/trace_dynevent.c-{
kernel/trace/trace_dynevent.c-	int ret;
kernel/trace/trace_dynevent.c-
kernel/trace/trace_dynevent.c:	ret = security_locked_down(LOCKDOWN_TRACEFS);
kernel/trace/trace_dynevent.c-	if (ret)
kernel/trace/trace_dynevent.c-		return ret;
kernel/trace/trace_dynevent.c-
--
kernel/trace/trace_printk.c-{
kernel/trace/trace_printk.c-	int ret;
kernel/trace/trace_printk.c-
kernel/trace/trace_printk.c:	ret = security_locked_down(LOCKDOWN_TRACEFS);
kernel/trace/trace_printk.c-	if (ret)
kernel/trace/trace_printk.c-		return ret;
kernel/trace/trace_printk.c-
--
kernel/trace/trace_fprobe.c-	int i, ret;
kernel/trace/trace_fprobe.c-
kernel/trace/trace_fprobe.c-	/* Should we need new LOCKDOWN flag for fprobe? */
kernel/trace/trace_fprobe.c:	ret = security_locked_down(LOCKDOWN_KPROBES);
kernel/trace/trace_fprobe.c-	if (ret)
kernel/trace/trace_fprobe.c-		return ret;
kernel/trace/trace_fprobe.c-
--
kernel/trace/trace_uprobe.c-{
kernel/trace/trace_uprobe.c-	int ret;
kernel/trace/trace_uprobe.c-
kernel/trace/trace_uprobe.c:	ret = security_locked_down(LOCKDOWN_TRACEFS);
kernel/trace/trace_uprobe.c-	if (ret)
kernel/trace/trace_uprobe.c-		return ret;
kernel/trace/trace_uprobe.c-
--
kernel/trace/trace_uprobe.c-{
kernel/trace/trace_uprobe.c-	int ret;
kernel/trace/trace_uprobe.c-
kernel/trace/trace_uprobe.c:	ret = security_locked_down(LOCKDOWN_TRACEFS);
kernel/trace/trace_uprobe.c-	if (ret)
kernel/trace/trace_uprobe.c-		return ret;
kernel/trace/trace_uprobe.c-
--
kernel/trace/trace_stack.c-{
kernel/trace/trace_stack.c-	int ret;
kernel/trace/trace_stack.c-
kernel/trace/trace_stack.c:	ret = security_locked_down(LOCKDOWN_TRACEFS);
kernel/trace/trace_stack.c-	if (ret)
kernel/trace/trace_stack.c-		return ret;
kernel/trace/trace_stack.c-
--
kernel/trace/trace_events_synth.c-{
kernel/trace/trace_events_synth.c-	int ret;
kernel/trace/trace_events_synth.c-
kernel/trace/trace_events_synth.c:	ret = security_locked_down(LOCKDOWN_TRACEFS);
kernel/trace/trace_events_synth.c-	if (ret)
kernel/trace/trace_events_synth.c-		return ret;
kernel/trace/trace_events_synth.c-
--
kernel/events/core.c-
kernel/events/core.c-	/* REGS_INTR can leak data, lockdown must prevent this */
kernel/events/core.c-	if (attr.sample_type & PERF_SAMPLE_REGS_INTR) {
kernel/events/core.c:		err = security_locked_down(LOCKDOWN_PERF);
kernel/events/core.c-		if (err)
kernel/events/core.c-			return err;
kernel/events/core.c-	}
--
kernel/kexec.c-	 * kexec can be used to circumvent module loading restrictions, so
kernel/kexec.c-	 * prevent loading in that case
kernel/kexec.c-	 */
kernel/kexec.c:	result = security_locked_down(LOCKDOWN_KEXEC);
kernel/kexec.c-	if (result)
kernel/kexec.c-		return result;
kernel/kexec.c-
--
kernel/bpf/helpers.c-	case BPF_FUNC_probe_read_user:
kernel/bpf/helpers.c-		return &bpf_probe_read_user_proto;
kernel/bpf/helpers.c-	case BPF_FUNC_probe_read_kernel:
kernel/bpf/helpers.c:		return security_locked_down(LOCKDOWN_BPF_READ_KERNEL) < 0 ?
kernel/bpf/helpers.c-		       NULL : &bpf_probe_read_kernel_proto;
kernel/bpf/helpers.c-	case BPF_FUNC_probe_read_user_str:
kernel/bpf/helpers.c-		return &bpf_probe_read_user_str_proto;
kernel/bpf/helpers.c-	case BPF_FUNC_probe_read_kernel_str:
kernel/bpf/helpers.c:		return security_locked_down(LOCKDOWN_BPF_READ_KERNEL) < 0 ?
kernel/bpf/helpers.c-		       NULL : &bpf_probe_read_kernel_str_proto;
kernel/bpf/helpers.c-	case BPF_FUNC_snprintf_btf:
kernel/bpf/helpers.c-		return &bpf_snprintf_btf_proto;
--
drivers/pci/syscall.c-	int err = 0;
drivers/pci/syscall.c-
drivers/pci/syscall.c-	if (!capable(CAP_SYS_ADMIN) ||
drivers/pci/syscall.c:	    security_locked_down(LOCKDOWN_PCI_ACCESS))
drivers/pci/syscall.c-		return -EPERM;
drivers/pci/syscall.c-
drivers/pci/syscall.c-	dev = pci_get_domain_bus_and_slot(0, bus, dfn);
--
drivers/pci/proc.c-	int size = dev->cfg_size;
drivers/pci/proc.c-	int cnt, ret;
drivers/pci/proc.c-
drivers/pci/proc.c:	ret = security_locked_down(LOCKDOWN_PCI_ACCESS);
drivers/pci/proc.c-	if (ret)
drivers/pci/proc.c-		return ret;
drivers/pci/proc.c-
--
drivers/pci/proc.c-#endif /* HAVE_PCI_MMAP */
drivers/pci/proc.c-	int ret = 0;
drivers/pci/proc.c-
drivers/pci/proc.c:	ret = security_locked_down(LOCKDOWN_PCI_ACCESS);
drivers/pci/proc.c-	if (ret)
drivers/pci/proc.c-		return ret;
drivers/pci/proc.c-
--
drivers/pci/proc.c-	int i, ret, write_combine = 0, res_bit = IORESOURCE_MEM;
drivers/pci/proc.c-
drivers/pci/proc.c-	if (!capable(CAP_SYS_RAWIO) ||
drivers/pci/proc.c:	    security_locked_down(LOCKDOWN_PCI_ACCESS))
drivers/pci/proc.c-		return -EPERM;
drivers/pci/proc.c-
drivers/pci/proc.c-	if (fpriv->mmap_state == pci_mmap_io) {
--
drivers/pci/pci-sysfs.c-	u8 *data = (u8 *) buf;
drivers/pci/pci-sysfs.c-	int ret;
drivers/pci/pci-sysfs.c-
drivers/pci/pci-sysfs.c:	ret = security_locked_down(LOCKDOWN_PCI_ACCESS);
drivers/pci/pci-sysfs.c-	if (ret)
drivers/pci/pci-sysfs.c-		return ret;
drivers/pci/pci-sysfs.c-
--
drivers/pci/pci-sysfs.c-	struct resource *res = &pdev->resource[bar];
drivers/pci/pci-sysfs.c-	int ret;
drivers/pci/pci-sysfs.c-
drivers/pci/pci-sysfs.c:	ret = security_locked_down(LOCKDOWN_PCI_ACCESS);
drivers/pci/pci-sysfs.c-	if (ret)
drivers/pci/pci-sysfs.c-		return ret;
drivers/pci/pci-sysfs.c-
--
drivers/pci/pci-sysfs.c-{
drivers/pci/pci-sysfs.c-	int ret;
drivers/pci/pci-sysfs.c-
drivers/pci/pci-sysfs.c:	ret = security_locked_down(LOCKDOWN_PCI_ACCESS);
drivers/pci/pci-sysfs.c-	if (ret)
drivers/pci/pci-sysfs.c-		return ret;
drivers/pci/pci-sysfs.c-
--
arch/powerpc/xmon/xmon.c-	static bool lockdown;
arch/powerpc/xmon/xmon.c-
arch/powerpc/xmon/xmon.c-	if (!lockdown) {
arch/powerpc/xmon/xmon.c:		lockdown = !!security_locked_down(LOCKDOWN_XMON_RW);
arch/powerpc/xmon/xmon.c-		if (lockdown) {
arch/powerpc/xmon/xmon.c-			printf("xmon: Disabled due to kernel lockdown\n");
arch/powerpc/xmon/xmon.c-			xmon_is_ro = true;
--
arch/powerpc/xmon/xmon.c-	}
arch/powerpc/xmon/xmon.c-
arch/powerpc/xmon/xmon.c-	if (!xmon_is_ro) {
arch/powerpc/xmon/xmon.c:		xmon_is_ro = !!security_locked_down(LOCKDOWN_XMON_WR);
arch/powerpc/xmon/xmon.c-		if (xmon_is_ro)
arch/powerpc/xmon/xmon.c-			printf("xmon: Read-only due to kernel lockdown\n");
arch/powerpc/xmon/xmon.c-	}
--
arch/s390/hypfs/hypfs_dbfs.c-{
arch/s390/hypfs/hypfs_dbfs.c-	const struct file_operations *fops = &dbfs_ops;
arch/s390/hypfs/hypfs_dbfs.c-
arch/s390/hypfs/hypfs_dbfs.c:	if (df->unlocked_ioctl && !security_locked_down(LOCKDOWN_DEBUGFS))
arch/s390/hypfs/hypfs_dbfs.c-		fops = &dbfs_ops_ioctl;
arch/s390/hypfs/hypfs_dbfs.c-	df->dentry = debugfs_create_file(df->name, 0400, dbfs_dir, df, fops);
arch/s390/hypfs/hypfs_dbfs.c-	mutex_init(&df->lock);
--
drivers/firmware/efi/efi.c-static char efivar_ssdt[EFIVAR_SSDT_NAME_MAX] __initdata;
drivers/firmware/efi/efi.c-static int __init efivar_ssdt_setup(char *str)
drivers/firmware/efi/efi.c-{
drivers/firmware/efi/efi.c:	int ret = security_locked_down(LOCKDOWN_ACPI_TABLES);
drivers/firmware/efi/efi.c-
drivers/firmware/efi/efi.c-	if (ret)
drivers/firmware/efi/efi.c-		return ret;
--
drivers/firmware/efi/test/efi_test.c-
drivers/firmware/efi/test/efi_test.c-static int efi_test_open(struct inode *inode, struct file *file)
drivers/firmware/efi/test/efi_test.c-{
drivers/firmware/efi/test/efi_test.c:	int ret = security_locked_down(LOCKDOWN_EFI_TEST);
drivers/firmware/efi/test/efi_test.c-
drivers/firmware/efi/test/efi_test.c-	if (ret)
drivers/firmware/efi/test/efi_test.c-		return ret;
--
arch/powerpc/platforms/pseries/reconfig.c-	char *kbuf;
arch/powerpc/platforms/pseries/reconfig.c-	char *tmp;
arch/powerpc/platforms/pseries/reconfig.c-
arch/powerpc/platforms/pseries/reconfig.c:	rv = security_locked_down(LOCKDOWN_DEVICE_TREE);
arch/powerpc/platforms/pseries/reconfig.c-	if (rv)
arch/powerpc/platforms/pseries/reconfig.c-		return rv;
arch/powerpc/platforms/pseries/reconfig.c-
--
arch/powerpc/kernel/rtas.c-	if (token_is_restricted_errinjct(token)) {
arch/powerpc/kernel/rtas.c-		/*
arch/powerpc/kernel/rtas.c-		 * It would be nicer to not discard the error value
arch/powerpc/kernel/rtas.c:		 * from security_locked_down(), but callers expect an
arch/powerpc/kernel/rtas.c-		 * RTAS status, not an errno.
arch/powerpc/kernel/rtas.c-		 */
arch/powerpc/kernel/rtas.c:		if (security_locked_down(LOCKDOWN_RTAS_ERROR_INJECTION))
arch/powerpc/kernel/rtas.c-			return -1;
arch/powerpc/kernel/rtas.c-	}
arch/powerpc/kernel/rtas.c-
--
arch/powerpc/kernel/rtas.c-	if (token_is_restricted_errinjct(token)) {
arch/powerpc/kernel/rtas.c-		int err;
arch/powerpc/kernel/rtas.c-
arch/powerpc/kernel/rtas.c:		err = security_locked_down(LOCKDOWN_RTAS_ERROR_INJECTION);
arch/powerpc/kernel/rtas.c-		if (err)
arch/powerpc/kernel/rtas.c-			return err;
arch/powerpc/kernel/rtas.c-	}
--
fs/debugfs/inode.c-	int ret;
fs/debugfs/inode.c-
fs/debugfs/inode.c-	if (ia->ia_valid & (ATTR_MODE | ATTR_UID | ATTR_GID)) {
fs/debugfs/inode.c:		ret = security_locked_down(LOCKDOWN_DEBUGFS);
fs/debugfs/inode.c-		if (ret)
fs/debugfs/inode.c-			return ret;
fs/debugfs/inode.c-	}
--
fs/debugfs/file.c-	    !real_fops->mmap)
fs/debugfs/file.c-		return 0;
fs/debugfs/file.c-
fs/debugfs/file.c:	if (security_locked_down(LOCKDOWN_DEBUGFS))
fs/debugfs/file.c-		return -EPERM;
fs/debugfs/file.c-
fs/debugfs/file.c-	return 0;
--
drivers/pcmcia/cistpl.c-	struct pcmcia_socket *s;
drivers/pcmcia/cistpl.c-	int error;
drivers/pcmcia/cistpl.c-
drivers/pcmcia/cistpl.c:	error = security_locked_down(LOCKDOWN_PCMCIA_CIS);
drivers/pcmcia/cistpl.c-	if (error)
drivers/pcmcia/cistpl.c-		return error;
drivers/pcmcia/cistpl.c-
--
arch/x86/mm/testmmiotrace.c-static int __init init(void)
arch/x86/mm/testmmiotrace.c-{
arch/x86/mm/testmmiotrace.c-	unsigned long size = (read_far) ? (8 << 20) : (16 << 10);
arch/x86/mm/testmmiotrace.c:	int ret = security_locked_down(LOCKDOWN_MMIOTRACE);
arch/x86/mm/testmmiotrace.c-
arch/x86/mm/testmmiotrace.c-	if (ret)
arch/x86/mm/testmmiotrace.c-		return ret;
--
fs/tracefs/inode.c-	struct dentry *dentry;
fs/tracefs/inode.c-	struct inode *inode;
fs/tracefs/inode.c-
fs/tracefs/inode.c:	if (security_locked_down(LOCKDOWN_TRACEFS))
fs/tracefs/inode.c-		return NULL;
fs/tracefs/inode.c-
fs/tracefs/inode.c-	if (!(mode & S_IFMT))
--
fs/tracefs/inode.c- */
fs/tracefs/inode.c-struct dentry *tracefs_create_dir(const char *name, struct dentry *parent)
fs/tracefs/inode.c-{
fs/tracefs/inode.c:	if (security_locked_down(LOCKDOWN_TRACEFS))
fs/tracefs/inode.c-		return NULL;
fs/tracefs/inode.c-
fs/tracefs/inode.c-	return __create_dir(name, parent, &tracefs_dir_inode_operations);
--
fs/tracefs/event_inode.c-	kuid_t uid;
fs/tracefs/event_inode.c-	kgid_t gid;
fs/tracefs/event_inode.c-
fs/tracefs/event_inode.c:	if (security_locked_down(LOCKDOWN_TRACEFS))
fs/tracefs/event_inode.c-		return NULL;
fs/tracefs/event_inode.c-
fs/tracefs/event_inode.c-	if (IS_ERR(dentry))
--
drivers/platform/x86/intel/tpmi.c-	 * /dev/mem interface is locked, don't allow debugfs to present any
drivers/platform/x86/intel/tpmi.c-	 * information. Also check for CAP_SYS_RAWIO as /dev/mem interface.
drivers/platform/x86/intel/tpmi.c-	 */
drivers/platform/x86/intel/tpmi.c:	if (!security_locked_down(LOCKDOWN_DEV_MEM) && capable(CAP_SYS_RAWIO))
drivers/platform/x86/intel/tpmi.c-		tpmi_dbgfs_register(tpmi_info);
drivers/platform/x86/intel/tpmi.c-
drivers/platform/x86/intel/tpmi.c-	return 0;
--
fs/proc/kcore.c-
fs/proc/kcore.c-static int open_kcore(struct inode *inode, struct file *filp)
fs/proc/kcore.c-{
fs/proc/kcore.c:	int ret = security_locked_down(LOCKDOWN_KCORE);
fs/proc/kcore.c-
fs/proc/kcore.c-	if (!capable(CAP_SYS_RAWIO))
fs/proc/kcore.c-		return -EPERM;
--
arch/x86/kernel/ioport.c-	if ((from + num <= from) || (from + num > IO_BITMAP_BITS))
arch/x86/kernel/ioport.c-		return -EINVAL;
arch/x86/kernel/ioport.c-	if (turn_on && (!capable(CAP_SYS_RAWIO) ||
arch/x86/kernel/ioport.c:			security_locked_down(LOCKDOWN_IOPORT)))
arch/x86/kernel/ioport.c-		return -EPERM;
arch/x86/kernel/ioport.c-
arch/x86/kernel/ioport.c-	/*
--
arch/x86/kernel/ioport.c-	/* Trying to gain more privileges? */
arch/x86/kernel/ioport.c-	if (level > old) {
arch/x86/kernel/ioport.c-		if (!capable(CAP_SYS_RAWIO) ||
arch/x86/kernel/ioport.c:		    security_locked_down(LOCKDOWN_IOPORT))
arch/x86/kernel/ioport.c-			return -EPERM;
arch/x86/kernel/ioport.c-	}
arch/x86/kernel/ioport.c-
--
arch/x86/kernel/msr.c-	int err = 0;
arch/x86/kernel/msr.c-	ssize_t bytes = 0;
arch/x86/kernel/msr.c-
arch/x86/kernel/msr.c:	err = security_locked_down(LOCKDOWN_MSR);
arch/x86/kernel/msr.c-	if (err)
arch/x86/kernel/msr.c-		return err;
arch/x86/kernel/msr.c-
--
arch/x86/kernel/msr.c-			err = -EFAULT;
arch/x86/kernel/msr.c-			break;
arch/x86/kernel/msr.c-		}
arch/x86/kernel/msr.c:		err = security_locked_down(LOCKDOWN_MSR);
arch/x86/kernel/msr.c-		if (err)
arch/x86/kernel/msr.c-			break;
arch/x86/kernel/msr.c-
--
drivers/acpi/custom_method.c-	acpi_status status;
drivers/acpi/custom_method.c-	int ret;
drivers/acpi/custom_method.c-
drivers/acpi/custom_method.c:	ret = security_locked_down(LOCKDOWN_ACPI_TABLES);
drivers/acpi/custom_method.c-	if (ret)
drivers/acpi/custom_method.c-		return ret;
drivers/acpi/custom_method.c-
--
drivers/acpi/osl.c-	 * specific location (if appropriate) so it can be carried
drivers/acpi/osl.c-	 * over further kexec()s.
drivers/acpi/osl.c-	 */
drivers/acpi/osl.c:	if (acpi_rsdp && !security_locked_down(LOCKDOWN_ACPI_TABLES)) {
drivers/acpi/osl.c-		acpi_arch_set_root_pointer(acpi_rsdp);
drivers/acpi/osl.c-		return acpi_rsdp;
drivers/acpi/osl.c-	}
--
drivers/acpi/acpi_configfs.c-{
drivers/acpi/acpi_configfs.c-	const struct acpi_table_header *header = data;
drivers/acpi/acpi_configfs.c-	struct acpi_table *table;
drivers/acpi/acpi_configfs.c:	int ret = security_locked_down(LOCKDOWN_ACPI_TABLES);
drivers/acpi/acpi_configfs.c-
drivers/acpi/acpi_configfs.c-	if (ret)
drivers/acpi/acpi_configfs.c-		return ret;
--
drivers/acpi/tables.c-	if (table_nr == 0)
drivers/acpi/tables.c-		return;
drivers/acpi/tables.c-
drivers/acpi/tables.c:	if (security_locked_down(LOCKDOWN_ACPI_TABLES)) {
drivers/acpi/tables.c-		pr_notice("kernel is locked down, ignoring table override\n");
drivers/acpi/tables.c-		return;
drivers/acpi/tables.c-	}
--
drivers/char/mem.c-	if (!capable(CAP_SYS_RAWIO))
drivers/char/mem.c-		return -EPERM;
drivers/char/mem.c-
drivers/char/mem.c:	rc = security_locked_down(LOCKDOWN_DEV_MEM);
drivers/char/mem.c-	if (rc)
drivers/char/mem.c-		return rc;
drivers/char/mem.c-
--
drivers/cxl/core/mbox.c-	if (!IS_ENABLED(CONFIG_CXL_MEM_RAW_COMMANDS))
drivers/cxl/core/mbox.c-		return false;
drivers/cxl/core/mbox.c-
drivers/cxl/core/mbox.c:	if (security_locked_down(LOCKDOWN_PCI_ACCESS))
drivers/cxl/core/mbox.c-		return false;
drivers/cxl/core/mbox.c-
drivers/cxl/core/mbox.c-	if (cxl_raw_allow_all)
--
drivers/tty/serial/serial_core.c-	}
drivers/tty/serial/serial_core.c-
drivers/tty/serial/serial_core.c-	if (change_irq || change_port) {
drivers/tty/serial/serial_core.c:		retval = security_locked_down(LOCKDOWN_TIOCSSERIAL);
drivers/tty/serial/serial_core.c-		if (retval)
drivers/tty/serial/serial_core.c-			goto exit;
drivers/tty/serial/serial_core.c-	}
```

---

# security hook: security_mmap_addr

mm/mmap.c:1
mm/nommu.c:1
arch/x86/kernel/vm86_32.c:2

```shell
mm/mmap.c-	if (offset_in_page(addr))
mm/mmap.c-		return -EINVAL;
mm/mmap.c-
mm/mmap.c:	error = security_mmap_addr(addr);
mm/mmap.c-	return error ? error : addr;
mm/mmap.c-}
mm/mmap.c-
--
mm/nommu.c-	}
mm/nommu.c-
mm/nommu.c-	/* allow the security API to have its say */
mm/nommu.c:	ret = security_mmap_addr(addr);
mm/nommu.c-	if (ret < 0)
mm/nommu.c-		return ret;
mm/nommu.c-
--
arch/x86/kernel/vm86_32.c-	unsigned long err = 0;
arch/x86/kernel/vm86_32.c-	struct vm86_struct v;
arch/x86/kernel/vm86_32.c-
arch/x86/kernel/vm86_32.c:	err = security_mmap_addr(0);
arch/x86/kernel/vm86_32.c-	if (err) {
arch/x86/kernel/vm86_32.c-		/*
arch/x86/kernel/vm86_32.c-		 * vm86 cannot virtualize the address space, so vm86 users
--
arch/x86/kernel/vm86_32.c-		 * To reduce the available kernel attack surface, simply
arch/x86/kernel/vm86_32.c-		 * disallow vm86(old) for users who cannot mmap at va 0.
arch/x86/kernel/vm86_32.c-		 *
arch/x86/kernel/vm86_32.c:		 * The implementation of security_mmap_addr will allow
arch/x86/kernel/vm86_32.c-		 * suitably privileged users to map va 0 even if
arch/x86/kernel/vm86_32.c-		 * vm.mmap_min_addr is set above 0, and we want this
arch/x86/kernel/vm86_32.c-		 * behavior for vm86 as well, as it ensures that legacy
```

---

# security hook: security_mmap_file

mm/gmem_util.c:1
mm/mmap.c:1
ipc/shm.c:1
mm/util.c:1

```shell
mm/gmem_util.c-	unsigned int retry_times = 0;
mm/gmem_util.c-
mm/gmem_util.c-retry:
mm/gmem_util.c:	ret = security_mmap_file(file, prot, flag);
mm/gmem_util.c-	if (!ret) {
mm/gmem_util.c-		if (mmap_write_lock_killable(mm)) {
mm/gmem_util.c-			gmem_release_vma(mm, &reserve_list);
--
mm/mmap.c-	mmap_read_unlock(mm);
mm/mmap.c-
mm/mmap.c-	/* Call outside mmap_lock to be consistent with other callers. */
mm/mmap.c:	ret = security_mmap_file(file, prot, flags);
mm/mmap.c-	if (ret) {
mm/mmap.c-		fput(file);
mm/mmap.c-		return ret;
--
ipc/shm.c-	sfd->vm_ops = NULL;
ipc/shm.c-	file->private_data = sfd;
ipc/shm.c-
ipc/shm.c:	err = security_mmap_file(file, prot, flags);
ipc/shm.c-	if (err)
ipc/shm.c-		goto out_fput;
ipc/shm.c-
--
mm/util.c-	if (gmem_is_enabled() && (flag & MAP_PEER_SHARED))
mm/util.c-		return gm_vm_mmap_pgoff(file, addr, len, prot, flag, pgoff);
mm/util.c-
mm/util.c:	ret = security_mmap_file(file, prot, flag);
mm/util.c-	if (!ret) {
mm/util.c-		if (mmap_write_lock_killable(mm))
mm/util.c-			return -EINTR;
```

---

# security hook: security_move_mount

fs/namespace.c:1

```shell
fs/namespace.c-	if (ret < 0)
fs/namespace.c-		goto out_from;
fs/namespace.c-
fs/namespace.c:	ret = security_move_mount(&from_path, &to_path);
fs/namespace.c-	if (ret < 0)
fs/namespace.c-		goto out_to;
fs/namespace.c-
```

---

# security hook: security_mptcp_add_subflow

net/mptcp/subflow.c:1

```shell
net/mptcp/subflow.c-
net/mptcp/subflow.c-	lock_sock_nested(sf->sk, SINGLE_DEPTH_NESTING);
net/mptcp/subflow.c-
net/mptcp/subflow.c:	err = security_mptcp_add_subflow(sk, sf->sk);
net/mptcp/subflow.c-	if (err)
net/mptcp/subflow.c-		goto err_free;
net/mptcp/subflow.c-
```

---

# security hook: security_msg_msg_alloc

ipc/msgutil.c:1

```shell
ipc/msgutil.c-			goto out_err;
ipc/msgutil.c-	}
ipc/msgutil.c-
ipc/msgutil.c:	err = security_msg_msg_alloc(msg);
ipc/msgutil.c-	if (err)
ipc/msgutil.c-		goto out_err;
ipc/msgutil.c-
```

---

# security hook: security_msg_msg_free

ipc/msgutil.c:1

```shell
ipc/msgutil.c-{
ipc/msgutil.c-	struct msg_msgseg *seg;
ipc/msgutil.c-
ipc/msgutil.c:	security_msg_msg_free(msg);
ipc/msgutil.c-
ipc/msgutil.c-	seg = msg->next;
ipc/msgutil.c-	kfree(msg);
```

---

# security hook: security_msg_queue_alloc

ipc/msg.c:1

```shell
ipc/msg.c-	msq->q_perm.key = key;
ipc/msg.c-
ipc/msg.c-	msq->q_perm.security = NULL;
ipc/msg.c:	retval = security_msg_queue_alloc(&msq->q_perm);
ipc/msg.c-	if (retval) {
ipc/msg.c-		kfree(msq);
ipc/msg.c-		return retval;
```

---

# security hook: security_msg_queue_associate

ipc/msg.c:1

```shell
ipc/msg.c-	struct ipc_namespace *ns;
ipc/msg.c-	static const struct ipc_ops msg_ops = {
ipc/msg.c-		.getnew = newque,
ipc/msg.c:		.associate = security_msg_queue_associate,
ipc/msg.c-	};
ipc/msg.c-	struct ipc_params msg_params;
ipc/msg.c-
```

---

# security hook: security_msg_queue_free

ipc/msg.c:1

```shell
ipc/msg.c-	struct kern_ipc_perm *p = container_of(head, struct kern_ipc_perm, rcu);
ipc/msg.c-	struct msg_queue *msq = container_of(p, struct msg_queue, q_perm);
ipc/msg.c-
ipc/msg.c:	security_msg_queue_free(&msq->q_perm);
ipc/msg.c-	kfree(msq);
ipc/msg.c-}
ipc/msg.c-
```

---

# security hook: security_msg_queue_msgctl

ipc/msg.c:3

```shell
ipc/msg.c-
ipc/msg.c-	msq = container_of(ipcp, struct msg_queue, q_perm);
ipc/msg.c-
ipc/msg.c:	err = security_msg_queue_msgctl(&msq->q_perm, cmd);
ipc/msg.c-	if (err)
ipc/msg.c-		goto out_unlock1;
ipc/msg.c-
--
ipc/msg.c-	 * due to padding, it's not enough
ipc/msg.c-	 * to set all member fields.
ipc/msg.c-	 */
ipc/msg.c:	err = security_msg_queue_msgctl(NULL, cmd);
ipc/msg.c-	if (err)
ipc/msg.c-		return err;
ipc/msg.c-
--
ipc/msg.c-			goto out_unlock;
ipc/msg.c-	}
ipc/msg.c-
ipc/msg.c:	err = security_msg_queue_msgctl(&msq->q_perm, cmd);
ipc/msg.c-	if (err)
ipc/msg.c-		goto out_unlock;
ipc/msg.c-
```

---

# security hook: security_msg_queue_msgrcv

ipc/msg.c:2

```shell
ipc/msg.c-
ipc/msg.c-	list_for_each_entry_safe(msr, t, &msq->q_receivers, r_list) {
ipc/msg.c-		if (testmsg(msg, msr->r_msgtype, msr->r_mode) &&
ipc/msg.c:		    !security_msg_queue_msgrcv(&msq->q_perm, msg, msr->r_tsk,
ipc/msg.c-					       msr->r_msgtype, msr->r_mode)) {
ipc/msg.c-
ipc/msg.c-			list_del(&msr->r_list);
--
ipc/msg.c-
ipc/msg.c-	list_for_each_entry(msg, &msq->q_messages, m_list) {
ipc/msg.c-		if (testmsg(msg, *msgtyp, mode) &&
ipc/msg.c:		    !security_msg_queue_msgrcv(&msq->q_perm, msg, current,
ipc/msg.c-					       *msgtyp, mode)) {
ipc/msg.c-			if (mode == SEARCH_LESSEQUAL && msg->m_type != 1) {
ipc/msg.c-				*msgtyp = msg->m_type - 1;
```

---

# security hook: security_msg_queue_msgsnd

ipc/msg.c:1

```shell
ipc/msg.c-			goto out_unlock0;
ipc/msg.c-		}
ipc/msg.c-
ipc/msg.c:		err = security_msg_queue_msgsnd(&msq->q_perm, msg, msgflg);
ipc/msg.c-		if (err)
ipc/msg.c-			goto out_unlock0;
ipc/msg.c-
```

---

# security hook: security_netlink_send

net/netlink/af_netlink.c:1

```shell
net/netlink/af_netlink.c-		goto out;
net/netlink/af_netlink.c-	}
net/netlink/af_netlink.c-
net/netlink/af_netlink.c:	err = security_netlink_send(sk, skb);
net/netlink/af_netlink.c-	if (err) {
net/netlink/af_netlink.c-		kfree_skb(skb);
net/netlink/af_netlink.c-		goto out;
```

---

# security hook: security_ops

Documentation/driver-api/nvdimm/security.rst:1
drivers/acpi/nfit/intel.c:2
drivers/acpi/nfit/intel.h:1
drivers/acpi/nfit/core.c:3
drivers/cxl/pmem.c:2
drivers/cxl/security.c:2
drivers/nvdimm/dimm_devs.c:1
drivers/nvdimm/nd-core.h:1

```shell
Documentation/driver-api/nvdimm/security.rst-With the introduction of Intel Device Specific Methods (DSM) v1.8
Documentation/driver-api/nvdimm/security.rst-specification [1], security DSMs are introduced. The spec added the following
Documentation/driver-api/nvdimm/security.rst-security DSMs: "get security state", "set passphrase", "disable passphrase",
Documentation/driver-api/nvdimm/security.rst:"unlock unit", "freeze lock", "secure erase", and "overwrite". A security_ops
Documentation/driver-api/nvdimm/security.rst-data structure has been added to struct dimm in order to support the security
Documentation/driver-api/nvdimm/security.rst-operations and generic APIs are exposed to allow vendor neutral operations.
Documentation/driver-api/nvdimm/security.rst-
--
drivers/cxl/pmem.c-#include "cxlmem.h"
drivers/cxl/pmem.c-#include "cxl.h"
drivers/cxl/pmem.c-
drivers/cxl/pmem.c:extern const struct nvdimm_security_ops *cxl_security_ops;
drivers/cxl/pmem.c-
drivers/cxl/pmem.c-static __read_mostly DECLARE_BITMAP(exclusive_cmds, CXL_MEM_COMMAND_ID_MAX);
drivers/cxl/pmem.c-
--
drivers/cxl/pmem.c-	nvdimm = __nvdimm_create(cxl_nvb->nvdimm_bus, cxl_nvd,
drivers/cxl/pmem.c-				 cxl_dimm_attribute_groups, flags,
drivers/cxl/pmem.c-				 cmd_mask, 0, NULL, cxl_nvd->dev_id,
drivers/cxl/pmem.c:				 cxl_security_ops, NULL);
drivers/cxl/pmem.c-	if (!nvdimm)
drivers/cxl/pmem.c-		return -ENOMEM;
drivers/cxl/pmem.c-
--
drivers/cxl/security.c-	return 0;
drivers/cxl/security.c-}
drivers/cxl/security.c-
drivers/cxl/security.c:static const struct nvdimm_security_ops __cxl_security_ops = {
drivers/cxl/security.c-	.get_flags = cxl_pmem_get_security_flags,
drivers/cxl/security.c-	.change_key = cxl_pmem_security_change_key,
drivers/cxl/security.c-	.disable = cxl_pmem_security_disable,
--
drivers/cxl/security.c-	.disable_master = cxl_pmem_security_disable_master,
drivers/cxl/security.c-};
drivers/cxl/security.c-
drivers/cxl/security.c:const struct nvdimm_security_ops *cxl_security_ops = &__cxl_security_ops;
--
drivers/acpi/nfit/intel.c-	}
drivers/acpi/nfit/intel.c-}
drivers/acpi/nfit/intel.c-
drivers/acpi/nfit/intel.c:static const struct nvdimm_security_ops __intel_security_ops = {
drivers/acpi/nfit/intel.c-	.get_flags = intel_security_flags,
drivers/acpi/nfit/intel.c-	.freeze = intel_security_freeze,
drivers/acpi/nfit/intel.c-	.change_key = intel_security_change_key,
--
drivers/acpi/nfit/intel.c-#endif
drivers/acpi/nfit/intel.c-};
drivers/acpi/nfit/intel.c-
drivers/acpi/nfit/intel.c:const struct nvdimm_security_ops *intel_security_ops = &__intel_security_ops;
drivers/acpi/nfit/intel.c-
drivers/acpi/nfit/intel.c-static int intel_bus_fwa_businfo(struct nvdimm_bus_descriptor *nd_desc,
drivers/acpi/nfit/intel.c-		struct nd_intel_bus_fw_activate_businfo *info)
--
drivers/acpi/nfit/intel.h-	};
drivers/acpi/nfit/intel.h-} __packed;
drivers/acpi/nfit/intel.h-
drivers/acpi/nfit/intel.h:extern const struct nvdimm_security_ops *intel_security_ops;
drivers/acpi/nfit/intel.h-
drivers/acpi/nfit/intel.h-#define ND_INTEL_STATUS_SIZE		4
drivers/acpi/nfit/intel.h-#define ND_INTEL_PASSPHRASE_SIZE	32
--
drivers/acpi/nfit/core.c-	mutex_unlock(&acpi_desc->init_mutex);
drivers/acpi/nfit/core.c-}
drivers/acpi/nfit/core.c-
drivers/acpi/nfit/core.c:static const struct nvdimm_security_ops *acpi_nfit_get_security_ops(int family)
drivers/acpi/nfit/core.c-{
drivers/acpi/nfit/core.c-	switch (family) {
drivers/acpi/nfit/core.c-	case NVDIMM_FAMILY_INTEL:
drivers/acpi/nfit/core.c:		return intel_security_ops;
drivers/acpi/nfit/core.c-	default:
drivers/acpi/nfit/core.c-		return NULL;
drivers/acpi/nfit/core.c-	}
--
drivers/acpi/nfit/core.c-				acpi_nfit_dimm_attribute_groups,
drivers/acpi/nfit/core.c-				flags, cmd_mask, flush ? flush->hint_count : 0,
drivers/acpi/nfit/core.c-				nfit_mem->flush_wpq, &nfit_mem->id[0],
drivers/acpi/nfit/core.c:				acpi_nfit_get_security_ops(nfit_mem->family),
drivers/acpi/nfit/core.c-				acpi_nfit_get_fw_ops(nfit_mem));
drivers/acpi/nfit/core.c-		if (!nvdimm)
drivers/acpi/nfit/core.c-			return -ENOMEM;
--
drivers/nvdimm/dimm_devs.c-		void *provider_data, const struct attribute_group **groups,
drivers/nvdimm/dimm_devs.c-		unsigned long flags, unsigned long cmd_mask, int num_flush,
drivers/nvdimm/dimm_devs.c-		struct resource *flush_wpq, const char *dimm_id,
drivers/nvdimm/dimm_devs.c:		const struct nvdimm_security_ops *sec_ops,
drivers/nvdimm/dimm_devs.c-		const struct nvdimm_fw_ops *fw_ops)
drivers/nvdimm/dimm_devs.c-{
drivers/nvdimm/dimm_devs.c-	struct nvdimm *nvdimm = kzalloc(sizeof(*nvdimm), GFP_KERNEL);
--
drivers/nvdimm/nd-core.h-	struct resource *flush_wpq;
drivers/nvdimm/nd-core.h-	const char *dimm_id;
drivers/nvdimm/nd-core.h-	struct {
drivers/nvdimm/nd-core.h:		const struct nvdimm_security_ops *ops;
drivers/nvdimm/nd-core.h-		unsigned long flags;
drivers/nvdimm/nd-core.h-		unsigned long ext_flags;
drivers/nvdimm/nd-core.h-		unsigned int overwrite_tmo;
```

---

# security hook: security_path_chmod

fs/open.c:1

```shell
fs/open.c-		return error;
fs/open.c-retry_deleg:
fs/open.c-	inode_lock(inode);
fs/open.c:	error = security_path_chmod(path, mode);
fs/open.c-	if (error)
fs/open.c-		goto out_unlock;
fs/open.c-	newattrs.ia_mode = (mode & S_IALLUGO) | (inode->i_mode & ~S_IALLUGO);
```

---

# security hook: security_path_chown

fs/open.c:1

```shell
fs/open.c-		newattrs.ia_valid |= ATTR_KILL_SUID | ATTR_KILL_PRIV |
fs/open.c-				     setattr_should_drop_sgid(idmap, inode);
fs/open.c-	/* Continue to send actual fs values, not the mount values. */
fs/open.c:	error = security_path_chown(
fs/open.c-		path,
fs/open.c-		from_vfsuid(idmap, fs_userns, newattrs.ia_vfsuid),
fs/open.c-		from_vfsgid(idmap, fs_userns, newattrs.ia_vfsgid));
```

---

# security hook: security_path_chroot

fs/open.c:1
fs/init.c:1

```shell
fs/init.c-	error = -EPERM;
fs/init.c-	if (!ns_capable(current_user_ns(), CAP_SYS_CHROOT))
fs/init.c-		goto dput_and_out;
fs/init.c:	error = security_path_chroot(&path);
fs/init.c-	if (error)
fs/init.c-		goto dput_and_out;
fs/init.c-	set_fs_root(current->fs, &path);
--
fs/open.c-	error = -EPERM;
fs/open.c-	if (!ns_capable(current_user_ns(), CAP_SYS_CHROOT))
fs/open.c-		goto dput_and_out;
fs/open.c:	error = security_path_chroot(&path);
fs/open.c-	if (error)
fs/open.c-		goto dput_and_out;
fs/open.c-
```

---

# security hook: security_path_link

fs/init.c:1
fs/namei.c:1

```shell
fs/namei.c-	error = may_linkat(idmap, &old_path);
fs/namei.c-	if (unlikely(error))
fs/namei.c-		goto out_dput;
fs/namei.c:	error = security_path_link(old_path.dentry, &new_path, new_dentry);
fs/namei.c-	if (error)
fs/namei.c-		goto out_dput;
fs/namei.c-	error = vfs_link(old_path.dentry, idmap, new_path.dentry->d_inode,
--
fs/init.c-	error = may_linkat(idmap, &old_path);
fs/init.c-	if (unlikely(error))
fs/init.c-		goto out_dput;
fs/init.c:	error = security_path_link(old_path.dentry, &new_path, new_dentry);
fs/init.c-	if (error)
fs/init.c-		goto out_dput;
fs/init.c-	error = vfs_link(old_path.dentry, idmap, new_path.dentry->d_inode,
```

---

# security hook: security_path_mkdir

fs/init.c:1
fs/cachefiles/namei.c:1
fs/namei.c:1

```shell
fs/init.c-		return PTR_ERR(dentry);
fs/init.c-	if (!IS_POSIXACL(path.dentry->d_inode))
fs/init.c-		mode &= ~current_umask();
fs/init.c:	error = security_path_mkdir(&path, dentry, mode);
fs/init.c-	if (!error)
fs/init.c-		error = vfs_mkdir(mnt_idmap(path.mnt), path.dentry->d_inode,
fs/init.c-				  dentry, mode);
--
fs/cachefiles/namei.c-
fs/cachefiles/namei.c-		path.mnt = cache->mnt;
fs/cachefiles/namei.c-		path.dentry = dir;
fs/cachefiles/namei.c:		ret = security_path_mkdir(&path, subdir, 0700);
fs/cachefiles/namei.c-		if (ret < 0)
fs/cachefiles/namei.c-			goto mkdir_error;
fs/cachefiles/namei.c-		ret = cachefiles_inject_write_error();
--
fs/namei.c-	if (IS_ERR(dentry))
fs/namei.c-		goto out_putname;
fs/namei.c-
fs/namei.c:	error = security_path_mkdir(&path, dentry,
fs/namei.c-			mode_strip_umask(path.dentry->d_inode, mode));
fs/namei.c-	if (!error) {
fs/namei.c-		error = vfs_mkdir(mnt_idmap(path.mnt), path.dentry->d_inode,
```

---

# security hook: security_path_mknod

kernel/bpf/inode.c:1
net/unix/af_unix.c:1
fs/namei.c:2
fs/init.c:1

```shell
kernel/bpf/inode.c-	}
kernel/bpf/inode.c-
kernel/bpf/inode.c-	mode = S_IFREG | ((S_IRUSR | S_IWUSR) & ~current_umask());
kernel/bpf/inode.c:	ret = security_path_mknod(&path, dentry, mode, 0);
kernel/bpf/inode.c-	if (ret)
kernel/bpf/inode.c-		goto out;
kernel/bpf/inode.c-
--
net/unix/af_unix.c-	 * All right, let's create it.
net/unix/af_unix.c-	 */
net/unix/af_unix.c-	idmap = mnt_idmap(parent.mnt);
net/unix/af_unix.c:	err = security_path_mknod(&parent, dentry, mode, 0);
net/unix/af_unix.c-	if (!err)
net/unix/af_unix.c-		err = vfs_mknod(idmap, d_inode(parent.dentry), dentry, mode, 0);
net/unix/af_unix.c-	if (err)
--
fs/namei.c-			const struct path *dir, struct dentry *dentry,
fs/namei.c-			umode_t mode)
fs/namei.c-{
fs/namei.c:	int error = security_path_mknod(dir, dentry, mode, 0);
fs/namei.c-	if (error)
fs/namei.c-		return error;
fs/namei.c-
--
fs/namei.c-	if (IS_ERR(dentry))
fs/namei.c-		goto out1;
fs/namei.c-
fs/namei.c:	error = security_path_mknod(&path, dentry,
fs/namei.c-			mode_strip_umask(path.dentry->d_inode, mode), dev);
fs/namei.c-	if (error)
fs/namei.c-		goto out2;
--
fs/init.c-
fs/init.c-	if (!IS_POSIXACL(path.dentry->d_inode))
fs/init.c-		mode &= ~current_umask();
fs/init.c:	error = security_path_mknod(&path, dentry, mode, dev);
fs/init.c-	if (!error)
fs/init.c-		error = vfs_mknod(mnt_idmap(path.mnt), path.dentry->d_inode,
fs/init.c-				  dentry, mode, new_decode_dev(dev));
```

---

# security hook: security_path_notify

fs/notify/fanotify/fanotify_user.c:1
fs/notify/dnotify/dnotify.c:1
fs/notify/inotify/inotify_user.c:1

```shell
fs/notify/fanotify/fanotify_user.c-		goto out;
fs/notify/fanotify/fanotify_user.c-	}
fs/notify/fanotify/fanotify_user.c-
fs/notify/fanotify/fanotify_user.c:	ret = security_path_notify(path, mask, obj_type);
fs/notify/fanotify/fanotify_user.c-	if (ret)
fs/notify/fanotify/fanotify_user.c-		path_put(path);
fs/notify/fanotify/fanotify_user.c-
--
fs/notify/dnotify/dnotify.c-	 */
fs/notify/dnotify/dnotify.c-	mask = convert_arg(arg);
fs/notify/dnotify/dnotify.c-
fs/notify/dnotify/dnotify.c:	error = security_path_notify(&filp->f_path, mask,
fs/notify/dnotify/dnotify.c-			FSNOTIFY_OBJ_TYPE_INODE);
fs/notify/dnotify/dnotify.c-	if (error)
fs/notify/dnotify/dnotify.c-		goto out_err;
--
fs/notify/inotify/inotify_user.c-		path_put(path);
fs/notify/inotify/inotify_user.c-		return error;
fs/notify/inotify/inotify_user.c-	}
fs/notify/inotify/inotify_user.c:	error = security_path_notify(path, mask,
fs/notify/inotify/inotify_user.c-				FSNOTIFY_OBJ_TYPE_INODE);
fs/notify/inotify/inotify_user.c-	if (error)
fs/notify/inotify/inotify_user.c-		path_put(path);
```

---

# security hook: security_path_rename

fs/cachefiles/namei.c:1
fs/namei.c:1

```shell
fs/cachefiles/namei.c-	path.dentry = dir;
fs/cachefiles/namei.c-	path_to_graveyard.mnt = cache->mnt;
fs/cachefiles/namei.c-	path_to_graveyard.dentry = cache->graveyard;
fs/cachefiles/namei.c:	ret = security_path_rename(&path, rep, &path_to_graveyard, grave, 0);
fs/cachefiles/namei.c-	if (ret < 0) {
fs/cachefiles/namei.c-		cachefiles_io_error(cache, "Rename security error %d", ret);
fs/cachefiles/namei.c-	} else {
--
fs/namei.c-	if (new_dentry == trap)
fs/namei.c-		goto exit5;
fs/namei.c-
fs/namei.c:	error = security_path_rename(&old_path, old_dentry,
fs/namei.c-				     &new_path, new_dentry, flags);
fs/namei.c-	if (error)
fs/namei.c-		goto exit5;
```

---

# security hook: security_path_rmdir

fs/namei.c:1

```shell
fs/namei.c-		error = -ENOENT;
fs/namei.c-		goto exit4;
fs/namei.c-	}
fs/namei.c:	error = security_path_rmdir(&path, dentry);
fs/namei.c-	if (error)
fs/namei.c-		goto exit4;
fs/namei.c-	error = vfs_rmdir(mnt_idmap(path.mnt), path.dentry->d_inode, dentry);
```

---

# security hook: security_path_symlink

fs/namei.c:1
fs/init.c:1

```shell
fs/init.c-	dentry = kern_path_create(AT_FDCWD, newname, &path, 0);
fs/init.c-	if (IS_ERR(dentry))
fs/init.c-		return PTR_ERR(dentry);
fs/init.c:	error = security_path_symlink(&path, dentry, oldname);
fs/init.c-	if (!error)
fs/init.c-		error = vfs_symlink(mnt_idmap(path.mnt), path.dentry->d_inode,
fs/init.c-				    dentry, oldname);
--
fs/namei.c-	if (IS_ERR(dentry))
fs/namei.c-		goto out_putnames;
fs/namei.c-
fs/namei.c:	error = security_path_symlink(&path, dentry, from->name);
fs/namei.c-	if (!error)
fs/namei.c-		error = vfs_symlink(mnt_idmap(path.mnt), path.dentry->d_inode,
fs/namei.c-				    dentry, from->name);
```

---

# security hook: security_path_truncate

kernel/trace/bpf_trace.c:1
fs/open.c:1

```shell
kernel/trace/bpf_trace.c-BTF_ID(func, security_file_open)
kernel/trace/bpf_trace.c-#endif
kernel/trace/bpf_trace.c-#ifdef CONFIG_SECURITY_PATH
kernel/trace/bpf_trace.c:BTF_ID(func, security_path_truncate)
kernel/trace/bpf_trace.c-#endif
kernel/trace/bpf_trace.c-BTF_ID(func, vfs_truncate)
kernel/trace/bpf_trace.c-BTF_ID(func, vfs_fallocate)
--
fs/open.c-	if (error)
fs/open.c-		goto put_write_and_out;
fs/open.c-
fs/open.c:	error = security_path_truncate(path);
fs/open.c-	if (!error)
fs/open.c-		error = do_truncate(idmap, path->dentry, length, 0, NULL);
fs/open.c-
```

---

# security hook: security_path_unlink

fs/namei.c:1
fs/cachefiles/namei.c:1

```shell
fs/cachefiles/namei.c-	int ret;
fs/cachefiles/namei.c-
fs/cachefiles/namei.c-	trace_cachefiles_unlink(object, d_inode(dentry)->i_ino, why);
fs/cachefiles/namei.c:	ret = security_path_unlink(&path, dentry);
fs/cachefiles/namei.c-	if (ret < 0) {
fs/cachefiles/namei.c-		cachefiles_io_error(cache, "Unlink security error");
fs/cachefiles/namei.c-		return ret;
--
fs/namei.c-		if (d_is_negative(dentry))
fs/namei.c-			goto slashes;
fs/namei.c-		ihold(inode);
fs/namei.c:		error = security_path_unlink(&path, dentry);
fs/namei.c-		if (error)
fs/namei.c-			goto exit3;
fs/namei.c-		error = vfs_unlink(mnt_idmap(path.mnt), path.dentry->d_inode,
```

---

# security hook: security_perf_event_alloc

kernel/events/core.c:1

```shell
kernel/events/core.c-		}
kernel/events/core.c-	}
kernel/events/core.c-
kernel/events/core.c:	err = security_perf_event_alloc(event);
kernel/events/core.c-	if (err)
kernel/events/core.c-		goto err_callchain_buffer;
kernel/events/core.c-
```

---

# security hook: security_perf_event_free

kernel/events/core.c:1

```shell
kernel/events/core.c-
kernel/events/core.c-	unaccount_event(event);
kernel/events/core.c-
kernel/events/core.c:	security_perf_event_free(event);
kernel/events/core.c-
kernel/events/core.c-	if (event->rb) {
kernel/events/core.c-		/*
```

---

# security hook: security_perf_event_open

kernel/events/core.c:2

```shell
kernel/events/core.c-		return err;
kernel/events/core.c-
kernel/events/core.c-	/* Do we allow access to perf_event_open(2) ? */
kernel/events/core.c:	err = security_perf_event_open(&attr, PERF_SECURITY_OPEN);
kernel/events/core.c-	if (err)
kernel/events/core.c-		return err;
kernel/events/core.c-
--
kernel/events/core.c-	if (sysctl_perf_event_paranoid > 1 && !perfmon_capable())
kernel/events/core.c-		return -EACCES;
kernel/events/core.c-
kernel/events/core.c:	return security_perf_event_open(attr, PERF_SECURITY_KERNEL);
kernel/events/core.c-}
kernel/events/core.c-EXPORT_SYMBOL_GPL(perf_allow_kernel);
kernel/events/core.c-
```

---

# security hook: security_perf_event_read

kernel/events/core.c:2

```shell
kernel/events/core.c-	struct perf_event_context *ctx;
kernel/events/core.c-	int ret;
kernel/events/core.c-
kernel/events/core.c:	ret = security_perf_event_read(event);
kernel/events/core.c-	if (ret)
kernel/events/core.c-		return ret;
kernel/events/core.c-
--
kernel/events/core.c-	if (!(vma->vm_flags & VM_SHARED))
kernel/events/core.c-		return -EINVAL;
kernel/events/core.c-
kernel/events/core.c:	ret = security_perf_event_read(event);
kernel/events/core.c-	if (ret)
kernel/events/core.c-		return ret;
kernel/events/core.c-
```

---

# security hook: security_perf_event_write

kernel/events/core.c:1

```shell
kernel/events/core.c-	long ret;
kernel/events/core.c-
kernel/events/core.c-	/* Treat ioctl like writes as it is likely a mutating operation. */
kernel/events/core.c:	ret = security_perf_event_write(event);
kernel/events/core.c-	if (ret)
kernel/events/core.c-		return ret;
kernel/events/core.c-
```

---

# security hook: security_post_notification

kernel/watch_queue.c:1

```shell
kernel/watch_queue.c-		if (wf && !filter_watch_notification(wf, n))
kernel/watch_queue.c-			continue;
kernel/watch_queue.c-
kernel/watch_queue.c:		if (security_post_notification(watch->cred, cred, n) < 0)
kernel/watch_queue.c-			continue;
kernel/watch_queue.c-
kernel/watch_queue.c-		if (lock_wqueue(wqueue)) {
```

---

# security hook: security_prepare_creds

kernel/cred.c:2
Documentation/trace/events.rst:3
Documentation/trace/histogram.rst:1

```shell
kernel/cred.c-	if (!new->ucounts)
kernel/cred.c-		goto error;
kernel/cred.c-
kernel/cred.c:	if (security_prepare_creds(new, old, GFP_KERNEL_ACCOUNT) < 0)
kernel/cred.c-		goto error;
kernel/cred.c-
kernel/cred.c-	return new;
--
kernel/cred.c-	if (!new->ucounts)
kernel/cred.c-		goto error;
kernel/cred.c-
kernel/cred.c:	if (security_prepare_creds(new, old, GFP_KERNEL_ACCOUNT) < 0)
kernel/cred.c-		goto error;
kernel/cred.c-
kernel/cred.c-	put_cred(old);
--
Documentation/trace/events.rst-
Documentation/trace/events.rst-You can convert any long type to a function address and search by function name::
Documentation/trace/events.rst-
Documentation/trace/events.rst:  call_site.function == security_prepare_creds
Documentation/trace/events.rst-
Documentation/trace/events.rst-The above will filter when the field "call_site" falls on the address within
Documentation/trace/events.rst:"security_prepare_creds". That is, it will compare the value of "call_site" and
Documentation/trace/events.rst-the filter will return true if it is greater than or equal to the start of
Documentation/trace/events.rst:the function "security_prepare_creds" and less than the end of that function.
Documentation/trace/events.rst-
Documentation/trace/events.rst-The ".function" postfix can only be attached to values of size long, and can only
Documentation/trace/events.rst-be compared with "==" or "!=".
--
Documentation/trace/histogram.rst-         kmem_cache_alloc_trace+0xeb/0x150
Documentation/trace/histogram.rst-         aa_alloc_task_context+0x27/0x40
Documentation/trace/histogram.rst-         apparmor_cred_prepare+0x1f/0x50
Documentation/trace/histogram.rst:         security_prepare_creds+0x16/0x20
Documentation/trace/histogram.rst-         prepare_creds+0xdf/0x1a0
Documentation/trace/histogram.rst-         SyS_capset+0xb5/0x200
Documentation/trace/histogram.rst-         system_call_fastpath+0x12/0x6a
```

---

# security hook: security_ptrace_access_check

kernel/ptrace.c:1

```shell
kernel/ptrace.c-	     !ptrace_has_cap(mm->user_ns, mode)))
kernel/ptrace.c-	    return -EPERM;
kernel/ptrace.c-
kernel/ptrace.c:	return security_ptrace_access_check(task, mode);
kernel/ptrace.c-}
kernel/ptrace.c-
kernel/ptrace.c-bool ptrace_may_access(struct task_struct *task, unsigned int mode)
```

---

# security hook: security_ptrace_traceme

kernel/ptrace.c:1

```shell
kernel/ptrace.c-	write_lock_irq(&tasklist_lock);
kernel/ptrace.c-	/* Are we already being traced? */
kernel/ptrace.c-	if (!current->ptrace) {
kernel/ptrace.c:		ret = security_ptrace_traceme(current->parent);
kernel/ptrace.c-		/*
kernel/ptrace.c-		 * Check PF_EXITING to ensure ->real_parent has not passed
kernel/ptrace.c-		 * exit_ptrace(). Otherwise we don't report the error but
```

---

# security hook: security_quotactl

fs/quota/quota.c:2

```shell
fs/quota/quota.c-			return -EPERM;
fs/quota/quota.c-	}
fs/quota/quota.c-
fs/quota/quota.c:	return security_quotactl(cmd, type, id, sb);
fs/quota/quota.c-}
fs/quota/quota.c-
fs/quota/quota.c-static void quota_sync_one(struct super_block *sb, void *arg)
--
fs/quota/quota.c-{
fs/quota/quota.c-	int ret;
fs/quota/quota.c-
fs/quota/quota.c:	ret = security_quotactl(Q_SYNC, type, 0, NULL);
fs/quota/quota.c-	if (!ret)
fs/quota/quota.c-		iterate_supers(quota_sync_one, &type);
fs/quota/quota.c-	return ret;
```

---

# security hook: security_quota_on

fs/quota/dquot.c:2

```shell
fs/quota/dquot.c-int dquot_quota_on(struct super_block *sb, int type, int format_id,
fs/quota/dquot.c-		   const struct path *path)
fs/quota/dquot.c-{
fs/quota/dquot.c:	int error = security_quota_on(path->dentry);
fs/quota/dquot.c-	if (error)
fs/quota/dquot.c-		return error;
fs/quota/dquot.c-	/* Quota file not on the same filesystem? */
--
fs/quota/dquot.c-	if (IS_ERR(dentry))
fs/quota/dquot.c-		return PTR_ERR(dentry);
fs/quota/dquot.c-
fs/quota/dquot.c:	error = security_quota_on(dentry);
fs/quota/dquot.c-	if (!error)
fs/quota/dquot.c-		error = dquot_load_quota_inode(d_inode(dentry), type, format_id,
fs/quota/dquot.c-				DQUOT_USAGE_ENABLED | DQUOT_LIMITS_ENABLED);
```

---

# security hook: security_release_secctx

net/ipv4/ip_sockglue.c:1
net/netfilter/nfnetlink_queue.c:2
net/netlabel/netlabel_unlabeled.c:4
net/netlabel/netlabel_user.c:1
net/netfilter/nf_conntrack_standalone.c:1
net/netfilter/nf_conntrack_netlink.c:1
kernel/audit.c:3
kernel/auditsc.c:3
fs/ceph/xattr.c:1
drivers/android/binder.c:2
fs/nfs/nfs4proc.c:1
fs/nfsd/nfs4xdr.c:1

```shell
net/netlabel/netlabel_unlabeled.c-					     &secctx,
net/netlabel/netlabel_unlabeled.c-					     &secctx_len) == 0) {
net/netlabel/netlabel_unlabeled.c-			audit_log_format(audit_buf, " sec_obj=%s", secctx);
net/netlabel/netlabel_unlabeled.c:			security_release_secctx(secctx, secctx_len);
net/netlabel/netlabel_unlabeled.c-		}
net/netlabel/netlabel_unlabeled.c-		audit_log_format(audit_buf, " res=%u", ret_val == 0 ? 1 : 0);
net/netlabel/netlabel_unlabeled.c-		audit_log_end(audit_buf);
--
net/netlabel/netlabel_unlabeled.c-		    security_secid_to_secctx(entry->secid,
net/netlabel/netlabel_unlabeled.c-					     &secctx, &secctx_len) == 0) {
net/netlabel/netlabel_unlabeled.c-			audit_log_format(audit_buf, " sec_obj=%s", secctx);
net/netlabel/netlabel_unlabeled.c:			security_release_secctx(secctx, secctx_len);
net/netlabel/netlabel_unlabeled.c-		}
net/netlabel/netlabel_unlabeled.c-		audit_log_format(audit_buf, " res=%u", entry != NULL ? 1 : 0);
net/netlabel/netlabel_unlabeled.c-		audit_log_end(audit_buf);
--
net/netlabel/netlabel_unlabeled.c-		    security_secid_to_secctx(entry->secid,
net/netlabel/netlabel_unlabeled.c-					     &secctx, &secctx_len) == 0) {
net/netlabel/netlabel_unlabeled.c-			audit_log_format(audit_buf, " sec_obj=%s", secctx);
net/netlabel/netlabel_unlabeled.c:			security_release_secctx(secctx, secctx_len);
net/netlabel/netlabel_unlabeled.c-		}
net/netlabel/netlabel_unlabeled.c-		audit_log_format(audit_buf, " res=%u", entry != NULL ? 1 : 0);
net/netlabel/netlabel_unlabeled.c-		audit_log_end(audit_buf);
--
net/netlabel/netlabel_unlabeled.c-			  NLBL_UNLABEL_A_SECCTX,
net/netlabel/netlabel_unlabeled.c-			  secctx_len,
net/netlabel/netlabel_unlabeled.c-			  secctx);
net/netlabel/netlabel_unlabeled.c:	security_release_secctx(secctx, secctx_len);
net/netlabel/netlabel_unlabeled.c-	if (ret_val != 0)
net/netlabel/netlabel_unlabeled.c-		goto list_cb_failure;
net/netlabel/netlabel_unlabeled.c-
--
net/netlabel/netlabel_user.c-				     &secctx,
net/netlabel/netlabel_user.c-				     &secctx_len) == 0) {
net/netlabel/netlabel_user.c-		audit_log_format(audit_buf, " subj=%s", secctx);
net/netlabel/netlabel_user.c:		security_release_secctx(secctx, secctx_len);
net/netlabel/netlabel_user.c-	}
net/netlabel/netlabel_user.c-
net/netlabel/netlabel_user.c-	return audit_buf;
--
net/ipv4/ip_sockglue.c-		return;
net/ipv4/ip_sockglue.c-
net/ipv4/ip_sockglue.c-	put_cmsg(msg, SOL_IP, SCM_SECURITY, seclen, secdata);
net/ipv4/ip_sockglue.c:	security_release_secctx(secdata, seclen);
net/ipv4/ip_sockglue.c-}
net/ipv4/ip_sockglue.c-
net/ipv4/ip_sockglue.c-static void ip_cmsg_recv_dstaddr(struct msghdr *msg, struct sk_buff *skb)
--
net/netfilter/nfnetlink_queue.c-
net/netfilter/nfnetlink_queue.c-	nlh->nlmsg_len = skb->len;
net/netfilter/nfnetlink_queue.c-	if (seclen)
net/netfilter/nfnetlink_queue.c:		security_release_secctx(secdata, seclen);
net/netfilter/nfnetlink_queue.c-	return skb;
net/netfilter/nfnetlink_queue.c-
net/netfilter/nfnetlink_queue.c-nla_put_failure:
--
net/netfilter/nfnetlink_queue.c-	net_err_ratelimited("nf_queue: error creating packet message\n");
net/netfilter/nfnetlink_queue.c-nlmsg_failure:
net/netfilter/nfnetlink_queue.c-	if (seclen)
net/netfilter/nfnetlink_queue.c:		security_release_secctx(secdata, seclen);
net/netfilter/nfnetlink_queue.c-	return NULL;
net/netfilter/nfnetlink_queue.c-}
net/netfilter/nfnetlink_queue.c-
--
net/netfilter/nf_conntrack_netlink.c-
net/netfilter/nf_conntrack_netlink.c-	ret = 0;
net/netfilter/nf_conntrack_netlink.c-nla_put_failure:
net/netfilter/nf_conntrack_netlink.c:	security_release_secctx(secctx, len);
net/netfilter/nf_conntrack_netlink.c-	return ret;
net/netfilter/nf_conntrack_netlink.c-}
net/netfilter/nf_conntrack_netlink.c-#else
--
net/netfilter/nf_conntrack_standalone.c-
net/netfilter/nf_conntrack_standalone.c-	seq_printf(s, "secctx=%s ", secctx);
net/netfilter/nf_conntrack_standalone.c-
net/netfilter/nf_conntrack_standalone.c:	security_release_secctx(secctx, len);
net/netfilter/nf_conntrack_standalone.c-}
net/netfilter/nf_conntrack_standalone.c-#else
net/netfilter/nf_conntrack_standalone.c-static inline void ct_show_secctx(struct seq_file *s, const struct nf_conn *ct)
--
kernel/auditsc.c-			rc = 1;
kernel/auditsc.c-		} else {
kernel/auditsc.c-			audit_log_format(ab, " obj=%s", ctx);
kernel/auditsc.c:			security_release_secctx(ctx, len);
kernel/auditsc.c-		}
kernel/auditsc.c-	}
kernel/auditsc.c-	audit_log_format(ab, " ocomm=");
--
kernel/auditsc.c-				*call_panic = 1;
kernel/auditsc.c-			} else {
kernel/auditsc.c-				audit_log_format(ab, " obj=%s", ctx);
kernel/auditsc.c:				security_release_secctx(ctx, len);
kernel/auditsc.c-			}
kernel/auditsc.c-		}
kernel/auditsc.c-		if (context->ipc.has_perm) {
--
kernel/auditsc.c-				*call_panic = 2;
kernel/auditsc.c-		} else {
kernel/auditsc.c-			audit_log_format(ab, " obj=%s", ctx);
kernel/auditsc.c:			security_release_secctx(ctx, len);
kernel/auditsc.c-		}
kernel/auditsc.c-	}
kernel/auditsc.c-
--
kernel/audit.c-		sig_data = kmalloc(struct_size(sig_data, ctx, len), GFP_KERNEL);
kernel/audit.c-		if (!sig_data) {
kernel/audit.c-			if (audit_sig_sid)
kernel/audit.c:				security_release_secctx(ctx, len);
kernel/audit.c-			return -ENOMEM;
kernel/audit.c-		}
kernel/audit.c-		sig_data->uid = from_kuid(&init_user_ns, audit_sig_uid);
kernel/audit.c-		sig_data->pid = audit_sig_pid;
kernel/audit.c-		if (audit_sig_sid) {
kernel/audit.c-			memcpy(sig_data->ctx, ctx, len);
kernel/audit.c:			security_release_secctx(ctx, len);
kernel/audit.c-		}
kernel/audit.c-		audit_send_reply(skb, seq, AUDIT_SIGNAL_INFO, 0, 0,
kernel/audit.c-				 sig_data, struct_size(sig_data, ctx, len));
--
kernel/audit.c-	}
kernel/audit.c-
kernel/audit.c-	audit_log_format(ab, " subj=%s", ctx);
kernel/audit.c:	security_release_secctx(ctx, len);
kernel/audit.c-	return 0;
kernel/audit.c-
kernel/audit.c-error_path:
--
fs/ceph/xattr.c-	posix_acl_release(as_ctx->default_acl);
fs/ceph/xattr.c-#endif
fs/ceph/xattr.c-#ifdef CONFIG_CEPH_FS_SECURITY_LABEL
fs/ceph/xattr.c:	security_release_secctx(as_ctx->sec_ctx, as_ctx->sec_ctxlen);
fs/ceph/xattr.c-#endif
fs/ceph/xattr.c-#ifdef CONFIG_FS_ENCRYPTION
fs/ceph/xattr.c-	kfree(as_ctx->fscrypt_auth);
--
fs/nfs/nfs4proc.c-nfs4_label_release_security(struct nfs4_label *label)
fs/nfs/nfs4proc.c-{
fs/nfs/nfs4proc.c-	if (label)
fs/nfs/nfs4proc.c:		security_release_secctx(label->label, label->len);
fs/nfs/nfs4proc.c-}
fs/nfs/nfs4proc.c-static inline u32 *nfs4_bitmask(struct nfs_server *server, struct nfs4_label *label)
fs/nfs/nfs4proc.c-{
--
fs/nfsd/nfs4xdr.c-out:
fs/nfsd/nfs4xdr.c-#ifdef CONFIG_NFSD_V4_SECURITY_LABEL
fs/nfsd/nfs4xdr.c-	if (context)
fs/nfsd/nfs4xdr.c:		security_release_secctx(context, contextlen);
fs/nfsd/nfs4xdr.c-#endif /* CONFIG_NFSD_V4_SECURITY_LABEL */
fs/nfsd/nfs4xdr.c-	kfree(acl);
fs/nfsd/nfs4xdr.c-	if (tempfh) {
--
drivers/android/binder.c-			t->security_ctx = 0;
drivers/android/binder.c-			WARN_ON(1);
drivers/android/binder.c-		}
drivers/android/binder.c:		security_release_secctx(secctx, secctx_sz);
drivers/android/binder.c-		secctx = NULL;
drivers/android/binder.c-	}
drivers/android/binder.c-	t->buffer->debug_id = t->debug_id;
--
drivers/android/binder.c-err_binder_alloc_buf_failed:
drivers/android/binder.c-err_bad_extra_size:
drivers/android/binder.c-	if (secctx)
drivers/android/binder.c:		security_release_secctx(secctx, secctx_sz);
drivers/android/binder.c-err_get_secctx_failed:
drivers/android/binder.c-	kfree(tcomplete);
drivers/android/binder.c-	binder_stats_deleted(BINDER_STAT_TRANSACTION_COMPLETE);
```

---

# security hook: security_req_classify_flow

net/ipv4/inet_connection_sock.c:2
net/dccp/ipv6.c:1
net/ipv4/syncookies.c:1
net/ipv6/inet6_connection_sock.c:1
net/ipv6/syncookies.c:1
drivers/net/ethernet/chelsio/inline_crypto/chtls/chtls_cm.c:1

```shell
net/ipv4/inet_connection_sock.c-			   (opt && opt->opt.srr) ? opt->opt.faddr : ireq->ir_rmt_addr,
net/ipv4/inet_connection_sock.c-			   ireq->ir_loc_addr, ireq->ir_rmt_port,
net/ipv4/inet_connection_sock.c-			   htons(ireq->ir_num), sk->sk_uid);
net/ipv4/inet_connection_sock.c:	security_req_classify_flow(req, flowi4_to_flowi_common(fl4));
net/ipv4/inet_connection_sock.c-	rt = ip_route_output_flow(net, fl4, sk);
net/ipv4/inet_connection_sock.c-	if (IS_ERR(rt))
net/ipv4/inet_connection_sock.c-		goto no_route;
--
net/ipv4/inet_connection_sock.c-			   (opt && opt->opt.srr) ? opt->opt.faddr : ireq->ir_rmt_addr,
net/ipv4/inet_connection_sock.c-			   ireq->ir_loc_addr, ireq->ir_rmt_port,
net/ipv4/inet_connection_sock.c-			   htons(ireq->ir_num), sk->sk_uid);
net/ipv4/inet_connection_sock.c:	security_req_classify_flow(req, flowi4_to_flowi_common(fl4));
net/ipv4/inet_connection_sock.c-	rt = ip_route_output_flow(net, fl4, sk);
net/ipv4/inet_connection_sock.c-	if (IS_ERR(rt))
net/ipv4/inet_connection_sock.c-		goto no_route;
--
net/ipv4/syncookies.c-			   IPPROTO_TCP, inet_sk_flowi_flags(sk),
net/ipv4/syncookies.c-			   opt->srr ? opt->faddr : ireq->ir_rmt_addr,
net/ipv4/syncookies.c-			   ireq->ir_loc_addr, th->source, th->dest, sk->sk_uid);
net/ipv4/syncookies.c:	security_req_classify_flow(req, flowi4_to_flowi_common(&fl4));
net/ipv4/syncookies.c-	rt = ip_route_output_key(sock_net(sk), &fl4);
net/ipv4/syncookies.c-	if (IS_ERR(rt)) {
net/ipv4/syncookies.c-		reqsk_free(req);
--
net/dccp/ipv6.c-	fl6.flowi6_oif = ireq->ir_iif;
net/dccp/ipv6.c-	fl6.fl6_dport = ireq->ir_rmt_port;
net/dccp/ipv6.c-	fl6.fl6_sport = htons(ireq->ir_num);
net/dccp/ipv6.c:	security_req_classify_flow(req, flowi6_to_flowi_common(&fl6));
net/dccp/ipv6.c-
net/dccp/ipv6.c-
net/dccp/ipv6.c-	rcu_read_lock();
--
net/ipv6/inet6_connection_sock.c-	fl6->fl6_dport = ireq->ir_rmt_port;
net/ipv6/inet6_connection_sock.c-	fl6->fl6_sport = htons(ireq->ir_num);
net/ipv6/inet6_connection_sock.c-	fl6->flowi6_uid = sk->sk_uid;
net/ipv6/inet6_connection_sock.c:	security_req_classify_flow(req, flowi6_to_flowi_common(fl6));
net/ipv6/inet6_connection_sock.c-
net/ipv6/inet6_connection_sock.c-	dst = ip6_dst_lookup_flow(sock_net(sk), sk, fl6, final_p);
net/ipv6/inet6_connection_sock.c-	if (IS_ERR(dst))
--
net/ipv6/syncookies.c-		fl6.fl6_dport = ireq->ir_rmt_port;
net/ipv6/syncookies.c-		fl6.fl6_sport = inet_sk(sk)->inet_sport;
net/ipv6/syncookies.c-		fl6.flowi6_uid = sk->sk_uid;
net/ipv6/syncookies.c:		security_req_classify_flow(req, flowi6_to_flowi_common(&fl6));
net/ipv6/syncookies.c-
net/ipv6/syncookies.c-		dst = ip6_dst_lookup_flow(sock_net(sk), sk, &fl6, final_p);
net/ipv6/syncookies.c-		if (IS_ERR(dst))
--
drivers/net/ethernet/chelsio/inline_crypto/chtls/chtls_cm.c-		fl6.daddr = ip6h->saddr;
drivers/net/ethernet/chelsio/inline_crypto/chtls/chtls_cm.c-		fl6.fl6_dport = inet_rsk(oreq)->ir_rmt_port;
drivers/net/ethernet/chelsio/inline_crypto/chtls/chtls_cm.c-		fl6.fl6_sport = htons(inet_rsk(oreq)->ir_num);
drivers/net/ethernet/chelsio/inline_crypto/chtls/chtls_cm.c:		security_req_classify_flow(oreq, flowi6_to_flowi_common(&fl6));
drivers/net/ethernet/chelsio/inline_crypto/chtls/chtls_cm.c-		dst = ip6_dst_lookup_flow(sock_net(lsk), lsk, &fl6, NULL);
drivers/net/ethernet/chelsio/inline_crypto/chtls/chtls_cm.c-		if (IS_ERR(dst))
drivers/net/ethernet/chelsio/inline_crypto/chtls/chtls_cm.c-			goto free_sk;
```

---

# security hook: security_sb_alloc

fs/super.c:1

```shell
fs/super.c-	 */
fs/super.c-	down_write_nested(&s->s_umount, SINGLE_DEPTH_NESTING);
fs/super.c-
fs/super.c:	if (security_sb_alloc(s))
fs/super.c-		goto fail;
fs/super.c-
fs/super.c-	for (i = 0; i < SB_FREEZE_LEVELS; i++) {
```

---

# security hook: security_sb_clone_mnt_opts

fs/nfs/getroot.c:1

```shell
fs/nfs/getroot.c-			goto error_splat_root;
fs/nfs/getroot.c-		}
fs/nfs/getroot.c-		/* clone lsm security options from the parent to the new sb */
fs/nfs/getroot.c:		error = security_sb_clone_mnt_opts(ctx->clone_data.sb,
fs/nfs/getroot.c-						   s, kflags, &kflags_out);
fs/nfs/getroot.c-		if (error)
fs/nfs/getroot.c-			goto error_splat_root;
```

---

# security hook: security_sb_delete

fs/super.c:1

```shell
fs/super.c-		 * to fsnotify or the security policy.
fs/super.c-		 */
fs/super.c-		fsnotify_sb_delete(sb);
fs/super.c:		security_sb_delete(sb);
fs/super.c-
fs/super.c-		/*
fs/super.c-		 * Now that all potentially-encrypted inodes have been evicted,
```

---

# security hook: security_sb_eat_lsm_opts

mm/shmem.c:1
fs/smb/client/fs_context.c:1
fs/fs_context.c:2
fs/btrfs/super.c:2

```shell
mm/shmem.c-	char *options = data;
mm/shmem.c-
mm/shmem.c-	if (options) {
mm/shmem.c:		int err = security_sb_eat_lsm_opts(options, &fc->security);
mm/shmem.c-		if (err)
mm/shmem.c-			return err;
mm/shmem.c-	}
--
fs/smb/client/fs_context.c-	if (!options)
fs/smb/client/fs_context.c-		return 0;
fs/smb/client/fs_context.c-
fs/smb/client/fs_context.c:	ret = security_sb_eat_lsm_opts(options, &fc->security);
fs/smb/client/fs_context.c-	if (ret)
fs/smb/client/fs_context.c-		return ret;
fs/smb/client/fs_context.c-
--
fs/fs_context.c-	if (!options)
fs/fs_context.c-		return 0;
fs/fs_context.c-
fs/fs_context.c:	ret = security_sb_eat_lsm_opts(options, &fc->security);
fs/fs_context.c-	if (ret)
fs/fs_context.c-		return ret;
fs/fs_context.c-
--
fs/fs_context.c-
fs/fs_context.c-	if (fc->fs_type->fs_flags & FS_BINARY_MOUNTDATA)
fs/fs_context.c-		return 0;
fs/fs_context.c:	return security_sb_eat_lsm_opts(ctx->legacy_data, &fc->security);
fs/fs_context.c-}
fs/fs_context.c-
fs/fs_context.c-/*
--
fs/btrfs/super.c-	int error = 0;
fs/btrfs/super.c-
fs/btrfs/super.c-	if (data) {
fs/btrfs/super.c:		error = security_sb_eat_lsm_opts(data, &new_sec_opts);
fs/btrfs/super.c-		if (error)
fs/btrfs/super.c-			return ERR_PTR(error);
fs/btrfs/super.c-	}
--
fs/btrfs/super.c-	if (data) {
fs/btrfs/super.c-		void *new_sec_opts = NULL;
fs/btrfs/super.c-
fs/btrfs/super.c:		ret = security_sb_eat_lsm_opts(data, &new_sec_opts);
fs/btrfs/super.c-		if (!ret)
fs/btrfs/super.c-			ret = security_sb_remount(sb, new_sec_opts);
fs/btrfs/super.c-		security_free_mnt_opts(&new_sec_opts);
```

---

# security hook: security_sb_free

fs/super.c:2

```shell
fs/super.c-	super_unlock_excl(s);
fs/super.c-	list_lru_destroy(&s->s_dentry_lru);
fs/super.c-	list_lru_destroy(&s->s_inode_lru);
fs/super.c:	security_sb_free(s);
fs/super.c-	put_user_ns(s->s_user_ns);
fs/super.c-	kfree(s->s_subtype);
fs/super.c-	shrinker_free(s->s_shrink);
--
fs/super.c-		WARN_ON(s->s_dentry_lru.node);
fs/super.c-		WARN_ON(s->s_inode_lru.node);
fs/super.c-		WARN_ON(!list_empty(&s->s_mounts));
fs/super.c:		security_sb_free(s);
fs/super.c-		put_user_ns(s->s_user_ns);
fs/super.c-		kfree(s->s_subtype);
fs/super.c-		call_rcu(&s->rcu, destroy_super_rcu);
```

---

# security hook: security_sb_kern_mount

fs/namespace.c:1
fs/fsopen.c:1

```shell
fs/namespace.c-	struct super_block *sb = fc->root->d_sb;
fs/namespace.c-	int error;
fs/namespace.c-
fs/namespace.c:	error = security_sb_kern_mount(sb);
fs/namespace.c-	if (!error && mount_too_revealing(sb, &mnt_flags))
fs/namespace.c-		error = -EPERM;
fs/namespace.c-
--
fs/fsopen.c-	}
fs/fsopen.c-
fs/fsopen.c-	sb = fc->root->d_sb;
fs/fsopen.c:	ret = security_sb_kern_mount(sb);
fs/fsopen.c-	if (unlikely(ret)) {
fs/fsopen.c-		fc_drop_locked(fc);
fs/fsopen.c-		fc->phase = FS_CONTEXT_FAILED;
```

---

# security hook: security_sb_mnt_opts_compat

fs/nfs/super.c:1

```shell
fs/nfs/super.c-	if (!nfs_compare_userns(old, server))
fs/nfs/super.c-		return 0;
fs/nfs/super.c-	if ((old->has_sec_mnt_opts || fc->security) &&
fs/nfs/super.c:			security_sb_mnt_opts_compat(sb, fc->security))
fs/nfs/super.c-		return 0;
fs/nfs/super.c-	return nfs_compare_mount_options(sb, server, fc);
fs/nfs/super.c-}
```

---

# security hook: security_sb_mount

Documentation/filesystems/mount_api.rst:1
fs/namespace.c:1

```shell
Documentation/filesystems/mount_api.rst-
Documentation/filesystems/mount_api.rst-   * ::
Documentation/filesystems/mount_api.rst-
Documentation/filesystems/mount_api.rst:	int security_sb_mountpoint(struct fs_context *fc,
Documentation/filesystems/mount_api.rst-			           struct path *mountpoint,
Documentation/filesystems/mount_api.rst-				   unsigned int mnt_flags);
Documentation/filesystems/mount_api.rst-
--
fs/namespace.c-	if (flags & MS_NOUSER)
fs/namespace.c-		return -EINVAL;
fs/namespace.c-
fs/namespace.c:	ret = security_sb_mount(dev_name, path, type_page, flags, data_page);
fs/namespace.c-	if (ret)
fs/namespace.c-		return ret;
fs/namespace.c-	if (!may_mount())
```

---

# security hook: security_sb_pivotroot

fs/namespace.c:1

```shell
fs/namespace.c-	if (error)
fs/namespace.c-		goto out1;
fs/namespace.c-
fs/namespace.c:	error = security_sb_pivotroot(&old, &new);
fs/namespace.c-	if (error)
fs/namespace.c-		goto out2;
fs/namespace.c-
```

---

# security hook: security_sb_remount

fs/super.c:1
fs/btrfs/super.c:1

```shell
fs/btrfs/super.c-
fs/btrfs/super.c-		ret = security_sb_eat_lsm_opts(data, &new_sec_opts);
fs/btrfs/super.c-		if (!ret)
fs/btrfs/super.c:			ret = security_sb_remount(sb, new_sec_opts);
fs/btrfs/super.c-		security_free_mnt_opts(&new_sec_opts);
fs/btrfs/super.c-		if (ret)
fs/btrfs/super.c-			goto restore;
--
fs/super.c-	if (sb->s_writers.frozen != SB_UNFROZEN)
fs/super.c-		return -EBUSY;
fs/super.c-
fs/super.c:	retval = security_sb_remount(sb, fc->security);
fs/super.c-	if (retval)
fs/super.c-		return retval;
fs/super.c-
```

---

# security hook: security_sb_set_mnt_opts

fs/nfs/getroot.c:1
fs/btrfs/super.c:1
fs/super.c:1

```shell
fs/nfs/getroot.c-		clone_server = NFS_SB(ctx->clone_data.sb);
fs/nfs/getroot.c-		server->has_sec_mnt_opts = clone_server->has_sec_mnt_opts;
fs/nfs/getroot.c-	} else {
fs/nfs/getroot.c:		error = security_sb_set_mnt_opts(s, fc->security,
fs/nfs/getroot.c-							kflags, &kflags_out);
fs/nfs/getroot.c-	}
fs/nfs/getroot.c-	if (error)
--
fs/btrfs/super.c-		error = btrfs_fill_super(s, fs_devices, data);
fs/btrfs/super.c-	}
fs/btrfs/super.c-	if (!error)
fs/btrfs/super.c:		error = security_sb_set_mnt_opts(s, new_sec_opts, 0, NULL);
fs/btrfs/super.c-	security_free_mnt_opts(&new_sec_opts);
fs/btrfs/super.c-	if (error) {
fs/btrfs/super.c-		deactivate_locked_super(s);
--
fs/super.c-	 */
fs/super.c-	super_wake(sb, SB_BORN);
fs/super.c-
fs/super.c:	error = security_sb_set_mnt_opts(sb, fc->security, 0, NULL);
fs/super.c-	if (unlikely(error)) {
fs/super.c-		fc_drop_locked(fc);
fs/super.c-		return error;
```

---

# security hook: security_sb_show_options

fs/proc_namespace.c:1

```shell
fs/proc_namespace.c-			seq_puts(m, fs_infop->str);
fs/proc_namespace.c-	}
fs/proc_namespace.c-
fs/proc_namespace.c:	return security_sb_show_options(m, sb);
fs/proc_namespace.c-}
fs/proc_namespace.c-
fs/proc_namespace.c-static void show_mnt_opts(struct seq_file *m, struct vfsmount *mnt)
```

---

# security hook: security_sb_statfs

fs/statfs.c:1

```shell
fs/statfs.c-		return -ENOSYS;
fs/statfs.c-
fs/statfs.c-	memset(buf, 0, sizeof(*buf));
fs/statfs.c:	retval = security_sb_statfs(dentry);
fs/statfs.c-	if (retval)
fs/statfs.c-		return retval;
fs/statfs.c-	retval = dentry->d_sb->s_op->statfs(dentry, buf);
```

---

# security hook: security_sb_umount

fs/namespace.c:1

```shell
fs/namespace.c-	struct super_block *sb = mnt->mnt.mnt_sb;
fs/namespace.c-	int retval;
fs/namespace.c-
fs/namespace.c:	retval = security_sb_umount(&mnt->mnt, flags);
fs/namespace.c-	if (retval)
fs/namespace.c-		return retval;
fs/namespace.c-
```

---

# security hook: security_sctp_assoc_established

net/sctp/sm_statefuns.c:1
Documentation/security/SCTP.rst:6

```shell
net/sctp/sm_statefuns.c-		return sctp_sf_pdiscard(net, ep, asoc, type, arg, commands);
net/sctp/sm_statefuns.c-
net/sctp/sm_statefuns.c-	/* Set peer label for connection. */
net/sctp/sm_statefuns.c:	if (security_sctp_assoc_established((struct sctp_association *)asoc,
net/sctp/sm_statefuns.c-					    chunk->head_skb ?: chunk->skb))
net/sctp/sm_statefuns.c-		return sctp_sf_pdiscard(net, ep, asoc, type, arg, commands);
net/sctp/sm_statefuns.c-
--
Documentation/security/SCTP.rst-    security_sctp_assoc_request()
Documentation/security/SCTP.rst-    security_sctp_bind_connect()
Documentation/security/SCTP.rst-    security_sctp_sk_clone()
Documentation/security/SCTP.rst:    security_sctp_assoc_established()
Documentation/security/SCTP.rst-
Documentation/security/SCTP.rst-The usage of these hooks are described below with the SELinux implementation
Documentation/security/SCTP.rst-described in the `SCTP SELinux Support`_ chapter.
--
Documentation/security/SCTP.rst-    @newsk - pointer to new sock structure.
Documentation/security/SCTP.rst-
Documentation/security/SCTP.rst-
Documentation/security/SCTP.rst:security_sctp_assoc_established()
Documentation/security/SCTP.rst-~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
Documentation/security/SCTP.rst-Called when a COOKIE ACK is received, and the peer secid will be
Documentation/security/SCTP.rst-saved into ``@asoc->peer_secid`` for client::
--
Documentation/security/SCTP.rst--------------------------------------------------
Documentation/security/SCTP.rst-
Documentation/security/SCTP.rst-The following diagram shows the use of ``security_sctp_bind_connect()``,
Documentation/security/SCTP.rst:``security_sctp_assoc_request()``, ``security_sctp_assoc_established()`` when
Documentation/security/SCTP.rst-establishing an association.
Documentation/security/SCTP.rst-::
Documentation/security/SCTP.rst-
--
Documentation/security/SCTP.rst-          <------------------------------------------- COOKIE ACK
Documentation/security/SCTP.rst-          |                                               |
Documentation/security/SCTP.rst-    sctp_sf_do_5_1E_ca                                    |
Documentation/security/SCTP.rst: Call security_sctp_assoc_established()                   |
Documentation/security/SCTP.rst- to set the peer label.                                   |
Documentation/security/SCTP.rst-          |                                               |
Documentation/security/SCTP.rst-          |                               If SCTP_SOCKET_TCP or peeled off
--
Documentation/security/SCTP.rst-    security_sctp_assoc_request()
Documentation/security/SCTP.rst-    security_sctp_bind_connect()
Documentation/security/SCTP.rst-    security_sctp_sk_clone()
Documentation/security/SCTP.rst:    security_sctp_assoc_established()
Documentation/security/SCTP.rst-
Documentation/security/SCTP.rst-
Documentation/security/SCTP.rst-security_sctp_assoc_request()
--
Documentation/security/SCTP.rst-    @newsk - pointer to new sock structure.
Documentation/security/SCTP.rst-
Documentation/security/SCTP.rst-
Documentation/security/SCTP.rst:security_sctp_assoc_established()
Documentation/security/SCTP.rst-~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
Documentation/security/SCTP.rst-Called when a COOKIE ACK is received where it sets the connection's peer sid
Documentation/security/SCTP.rst-to that in ``@skb``::
```

---

# security hook: security_sctp_assoc_request

net/sctp/sm_statefuns.c:5
Documentation/security/SCTP.rst:7

```shell
net/sctp/sm_statefuns.c-		goto nomem;
net/sctp/sm_statefuns.c-
net/sctp/sm_statefuns.c-	/* Update socket peer label if first association. */
net/sctp/sm_statefuns.c:	if (security_sctp_assoc_request(new_asoc, chunk->skb)) {
net/sctp/sm_statefuns.c-		sctp_association_free(new_asoc);
net/sctp/sm_statefuns.c-		return sctp_sf_pdiscard(net, ep, asoc, type, arg, commands);
net/sctp/sm_statefuns.c-	}
--
net/sctp/sm_statefuns.c-		}
net/sctp/sm_statefuns.c-	}
net/sctp/sm_statefuns.c-
net/sctp/sm_statefuns.c:	if (security_sctp_assoc_request(new_asoc, chunk->head_skb ?: chunk->skb)) {
net/sctp/sm_statefuns.c-		sctp_association_free(new_asoc);
net/sctp/sm_statefuns.c-		return sctp_sf_pdiscard(net, ep, asoc, type, arg, commands);
net/sctp/sm_statefuns.c-	}
--
net/sctp/sm_statefuns.c-		goto nomem;
net/sctp/sm_statefuns.c-
net/sctp/sm_statefuns.c-	/* Update socket peer label if first association. */
net/sctp/sm_statefuns.c:	if (security_sctp_assoc_request(new_asoc, chunk->skb)) {
net/sctp/sm_statefuns.c-		sctp_association_free(new_asoc);
net/sctp/sm_statefuns.c-		return sctp_sf_pdiscard(net, ep, asoc, type, arg, commands);
net/sctp/sm_statefuns.c-	}
--
net/sctp/sm_statefuns.c-	action = sctp_tietags_compare(new_asoc, asoc);
net/sctp/sm_statefuns.c-
net/sctp/sm_statefuns.c-	/* In cases C and E the association doesn't enter the ESTABLISHED
net/sctp/sm_statefuns.c:	 * state, so there is no need to call security_sctp_assoc_request().
net/sctp/sm_statefuns.c-	 */
net/sctp/sm_statefuns.c-	switch (action) {
net/sctp/sm_statefuns.c-	case 'A': /* Association restart. */
net/sctp/sm_statefuns.c-	case 'B': /* Collision case B. */
net/sctp/sm_statefuns.c-	case 'D': /* Collision case D. */
net/sctp/sm_statefuns.c-		/* Update socket peer label if first association. */
net/sctp/sm_statefuns.c:		if (security_sctp_assoc_request((struct sctp_association *)asoc,
net/sctp/sm_statefuns.c-						chunk->head_skb ?: chunk->skb)) {
net/sctp/sm_statefuns.c-			sctp_association_free(new_asoc);
net/sctp/sm_statefuns.c-			return sctp_sf_pdiscard(net, ep, asoc, type, arg, commands);
--
Documentation/security/SCTP.rst-
Documentation/security/SCTP.rst-For security module support, three SCTP specific hooks have been implemented::
Documentation/security/SCTP.rst-
Documentation/security/SCTP.rst:    security_sctp_assoc_request()
Documentation/security/SCTP.rst-    security_sctp_bind_connect()
Documentation/security/SCTP.rst-    security_sctp_sk_clone()
Documentation/security/SCTP.rst-    security_sctp_assoc_established()
--
Documentation/security/SCTP.rst-described in the `SCTP SELinux Support`_ chapter.
Documentation/security/SCTP.rst-
Documentation/security/SCTP.rst-
Documentation/security/SCTP.rst:security_sctp_assoc_request()
Documentation/security/SCTP.rst-~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
Documentation/security/SCTP.rst-Passes the ``@asoc`` and ``@chunk->skb`` of the association INIT packet to the
Documentation/security/SCTP.rst-security module. Returns 0 on success, error on failure.
--
Documentation/security/SCTP.rst--------------------------------------------------
Documentation/security/SCTP.rst-
Documentation/security/SCTP.rst-The following diagram shows the use of ``security_sctp_bind_connect()``,
Documentation/security/SCTP.rst:``security_sctp_assoc_request()``, ``security_sctp_assoc_established()`` when
Documentation/security/SCTP.rst-establishing an association.
Documentation/security/SCTP.rst-::
Documentation/security/SCTP.rst-
--
Documentation/security/SCTP.rst-                                                 Respond to an INIT chunk.
Documentation/security/SCTP.rst-                                             SCTP peer endpoint "A" is asking
Documentation/security/SCTP.rst-                                             for a temporary association.
Documentation/security/SCTP.rst:                                             Call security_sctp_assoc_request()
Documentation/security/SCTP.rst-                                             to set the peer label if first
Documentation/security/SCTP.rst-                                             association.
Documentation/security/SCTP.rst-                                             If not first association, check
--
Documentation/security/SCTP.rst-                                             Respond to an COOKIE ECHO chunk.
Documentation/security/SCTP.rst-                                             Confirm the cookie and create a
Documentation/security/SCTP.rst-                                             permanent association.
Documentation/security/SCTP.rst:                                             Call security_sctp_assoc_request() to
Documentation/security/SCTP.rst-                                             do the same as for INIT chunk Response.
Documentation/security/SCTP.rst-          <------------------------------------------- COOKIE ACK
Documentation/security/SCTP.rst-          |                                               |
--
Documentation/security/SCTP.rst-The `SCTP LSM Support`_ chapter above describes the following SCTP security
Documentation/security/SCTP.rst-hooks with the SELinux specifics expanded below::
Documentation/security/SCTP.rst-
Documentation/security/SCTP.rst:    security_sctp_assoc_request()
Documentation/security/SCTP.rst-    security_sctp_bind_connect()
Documentation/security/SCTP.rst-    security_sctp_sk_clone()
Documentation/security/SCTP.rst-    security_sctp_assoc_established()
Documentation/security/SCTP.rst-
Documentation/security/SCTP.rst-
Documentation/security/SCTP.rst:security_sctp_assoc_request()
Documentation/security/SCTP.rst-~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
Documentation/security/SCTP.rst-Passes the ``@asoc`` and ``@chunk->skb`` of the association INIT packet to the
Documentation/security/SCTP.rst-security module. Returns 0 on success, error on failure.
```

---

# security hook: security_sctp_bind_connect

net/sctp/sm_make_chunk.c:2
net/sctp/socket.c:5
Documentation/security/SCTP.rst:6

```shell
net/sctp/sm_make_chunk.c-		if (af->is_any(&addr))
net/sctp/sm_make_chunk.c-			memcpy(&addr, &asconf->source, sizeof(addr));
net/sctp/sm_make_chunk.c-
net/sctp/sm_make_chunk.c:		if (security_sctp_bind_connect(asoc->ep->base.sk,
net/sctp/sm_make_chunk.c-					       SCTP_PARAM_ADD_IP,
net/sctp/sm_make_chunk.c-					       (struct sockaddr *)&addr,
net/sctp/sm_make_chunk.c-					       af->sockaddr_len))
--
net/sctp/sm_make_chunk.c-		if (af->is_any(&addr))
net/sctp/sm_make_chunk.c-			memcpy(&addr, sctp_source(asconf), sizeof(addr));
net/sctp/sm_make_chunk.c-
net/sctp/sm_make_chunk.c:		if (security_sctp_bind_connect(asoc->ep->base.sk,
net/sctp/sm_make_chunk.c-					       SCTP_PARAM_SET_PRIMARY,
net/sctp/sm_make_chunk.c-					       (struct sockaddr *)&addr,
net/sctp/sm_make_chunk.c-					       af->sockaddr_len))
--
net/sctp/socket.c-	switch (op) {
net/sctp/socket.c-	case SCTP_BINDX_ADD_ADDR:
net/sctp/socket.c-		/* Allow security module to validate bindx addresses. */
net/sctp/socket.c:		err = security_sctp_bind_connect(sk, SCTP_SOCKOPT_BINDX_ADD,
net/sctp/socket.c-						 addrs, addrs_size);
net/sctp/socket.c-		if (err)
net/sctp/socket.c-			return err;
--
net/sctp/socket.c-		return -EINVAL;
net/sctp/socket.c-
net/sctp/socket.c-	/* Allow security module to validate connectx addresses. */
net/sctp/socket.c:	err = security_sctp_bind_connect(sk, SCTP_SOCKOPT_CONNECTX,
net/sctp/socket.c-					 (struct sockaddr *)kaddrs,
net/sctp/socket.c-					  addrs_size);
net/sctp/socket.c-	if (err)
--
net/sctp/socket.c-	af = sctp_get_af_specific(daddr->sa.sa_family);
net/sctp/socket.c-	if (!af)
net/sctp/socket.c-		return -EINVAL;
net/sctp/socket.c:	err = security_sctp_bind_connect(sk, SCTP_SENDMSG_CONNECT,
net/sctp/socket.c-					 (struct sockaddr *)daddr,
net/sctp/socket.c-					 af->sockaddr_len);
net/sctp/socket.c-	if (err < 0)
--
net/sctp/socket.c-	if (!af)
net/sctp/socket.c-		return -EINVAL;
net/sctp/socket.c-
net/sctp/socket.c:	err = security_sctp_bind_connect(sk, SCTP_PRIMARY_ADDR,
net/sctp/socket.c-					 (struct sockaddr *)&prim->ssp_addr,
net/sctp/socket.c-					 af->sockaddr_len);
net/sctp/socket.c-	if (err)
--
net/sctp/socket.c-		return -EADDRNOTAVAIL;
net/sctp/socket.c-
net/sctp/socket.c-	/* Allow security module to validate address. */
net/sctp/socket.c:	err = security_sctp_bind_connect(sk, SCTP_SET_PEER_PRIMARY_ADDR,
net/sctp/socket.c-					 (struct sockaddr *)&prim->sspp_addr,
net/sctp/socket.c-					 af->sockaddr_len);
net/sctp/socket.c-	if (err)
--
Documentation/security/SCTP.rst-For security module support, three SCTP specific hooks have been implemented::
Documentation/security/SCTP.rst-
Documentation/security/SCTP.rst-    security_sctp_assoc_request()
Documentation/security/SCTP.rst:    security_sctp_bind_connect()
Documentation/security/SCTP.rst-    security_sctp_sk_clone()
Documentation/security/SCTP.rst-    security_sctp_assoc_established()
Documentation/security/SCTP.rst-
--
Documentation/security/SCTP.rst-    @skb - pointer to skbuff of association packet.
Documentation/security/SCTP.rst-
Documentation/security/SCTP.rst-
Documentation/security/SCTP.rst:security_sctp_bind_connect()
Documentation/security/SCTP.rst-~~~~~~~~~~~~~~~~~~~~~~~~~~~~
Documentation/security/SCTP.rst-Passes one or more ipv4/ipv6 addresses to the security module for validation
Documentation/security/SCTP.rst-based on the ``@optname`` that will result in either a bind or connect
--
Documentation/security/SCTP.rst-Security Hooks used for Association Establishment
Documentation/security/SCTP.rst--------------------------------------------------
Documentation/security/SCTP.rst-
Documentation/security/SCTP.rst:The following diagram shows the use of ``security_sctp_bind_connect()``,
Documentation/security/SCTP.rst-``security_sctp_assoc_request()``, ``security_sctp_assoc_established()`` when
Documentation/security/SCTP.rst-establishing an association.
Documentation/security/SCTP.rst-::
--
Documentation/security/SCTP.rst- by a connect(2), sctp_connectx(3),
Documentation/security/SCTP.rst- sendmsg(2) or sctp_sendmsg(3).
Documentation/security/SCTP.rst- These will result in a call to
Documentation/security/SCTP.rst: security_sctp_bind_connect() to
Documentation/security/SCTP.rst- initiate an association to
Documentation/security/SCTP.rst- SCTP peer endpoint "Z".
Documentation/security/SCTP.rst-         INIT --------------------------------------------->
--
Documentation/security/SCTP.rst-hooks with the SELinux specifics expanded below::
Documentation/security/SCTP.rst-
Documentation/security/SCTP.rst-    security_sctp_assoc_request()
Documentation/security/SCTP.rst:    security_sctp_bind_connect()
Documentation/security/SCTP.rst-    security_sctp_sk_clone()
Documentation/security/SCTP.rst-    security_sctp_assoc_established()
Documentation/security/SCTP.rst-
--
Documentation/security/SCTP.rst-     options are set on the socket.
Documentation/security/SCTP.rst-
Documentation/security/SCTP.rst-
Documentation/security/SCTP.rst:security_sctp_bind_connect()
Documentation/security/SCTP.rst-~~~~~~~~~~~~~~~~~~~~~~~~~~~~
Documentation/security/SCTP.rst-Checks permissions required for ipv4/ipv6 addresses based on the ``@optname``
Documentation/security/SCTP.rst-as follows::
```

---

# security hook: security_sctp_sk_clone

net/sctp/socket.c:1
Documentation/security/SCTP.rst:6

```shell
net/sctp/socket.c-	/* Set newsk security attributes from original sk and connection
net/sctp/socket.c-	 * security attribute from asoc.
net/sctp/socket.c-	 */
net/sctp/socket.c:	security_sctp_sk_clone(asoc, sk, newsk);
net/sctp/socket.c-}
net/sctp/socket.c-
net/sctp/socket.c-static inline void sctp_copy_descendant(struct sock *sk_to,
--
Documentation/security/SCTP.rst-
Documentation/security/SCTP.rst-    security_sctp_assoc_request()
Documentation/security/SCTP.rst-    security_sctp_bind_connect()
Documentation/security/SCTP.rst:    security_sctp_sk_clone()
Documentation/security/SCTP.rst-    security_sctp_assoc_established()
Documentation/security/SCTP.rst-
Documentation/security/SCTP.rst-The usage of these hooks are described below with the SELinux implementation
--
Documentation/security/SCTP.rst-    SCTP_SET_PEER_PRIMARY_ADDR ->   SCTP_PARAM_SET_PRIMARY
Documentation/security/SCTP.rst-
Documentation/security/SCTP.rst-
Documentation/security/SCTP.rst:security_sctp_sk_clone()
Documentation/security/SCTP.rst-~~~~~~~~~~~~~~~~~~~~~~~~
Documentation/security/SCTP.rst-Called whenever a new socket is created by **accept**\(2)
Documentation/security/SCTP.rst-(i.e. a TCP style socket) or when a socket is 'peeled off' e.g userspace
--
Documentation/security/SCTP.rst- to set the peer label.                                   |
Documentation/security/SCTP.rst-          |                                               |
Documentation/security/SCTP.rst-          |                               If SCTP_SOCKET_TCP or peeled off
Documentation/security/SCTP.rst:          |                               socket security_sctp_sk_clone() is
Documentation/security/SCTP.rst-          |                               called to clone the new socket.
Documentation/security/SCTP.rst-          |                                               |
Documentation/security/SCTP.rst-      ESTABLISHED                                    ESTABLISHED
--
Documentation/security/SCTP.rst-
Documentation/security/SCTP.rst-    security_sctp_assoc_request()
Documentation/security/SCTP.rst-    security_sctp_bind_connect()
Documentation/security/SCTP.rst:    security_sctp_sk_clone()
Documentation/security/SCTP.rst-    security_sctp_assoc_established()
Documentation/security/SCTP.rst-
Documentation/security/SCTP.rst-
--
Documentation/security/SCTP.rst-Reconfiguration is enabled.
Documentation/security/SCTP.rst-
Documentation/security/SCTP.rst-
Documentation/security/SCTP.rst:security_sctp_sk_clone()
Documentation/security/SCTP.rst-~~~~~~~~~~~~~~~~~~~~~~~~
Documentation/security/SCTP.rst-Called whenever a new socket is created by **accept**\(2) (i.e. a TCP style
Documentation/security/SCTP.rst-socket) or when a socket is 'peeled off' e.g userspace calls
Documentation/security/SCTP.rst:**sctp_peeloff**\(3). ``security_sctp_sk_clone()`` will set the new
Documentation/security/SCTP.rst-sockets sid and peer sid to that contained in the ``@asoc sid`` and
Documentation/security/SCTP.rst-``@asoc peer sid`` respectively.
Documentation/security/SCTP.rst-::
```

---

# security hook: security_secctx_to_secid

kernel/cred.c:1
net/netlabel/netlabel_unlabeled.c:2
net/netfilter/nft_meta.c:1
net/netfilter/xt_SECMARK.c:1
fs/cachefiles/daemon.c:1

```shell
net/netlabel/netlabel_unlabeled.c-	if (ret_val != 0)
net/netlabel/netlabel_unlabeled.c-		return ret_val;
net/netlabel/netlabel_unlabeled.c-	dev_name = nla_data(info->attrs[NLBL_UNLABEL_A_IFACE]);
net/netlabel/netlabel_unlabeled.c:	ret_val = security_secctx_to_secid(
net/netlabel/netlabel_unlabeled.c-		                  nla_data(info->attrs[NLBL_UNLABEL_A_SECCTX]),
net/netlabel/netlabel_unlabeled.c-				  nla_len(info->attrs[NLBL_UNLABEL_A_SECCTX]),
net/netlabel/netlabel_unlabeled.c-				  &secid);
--
net/netlabel/netlabel_unlabeled.c-	ret_val = netlbl_unlabel_addrinfo_get(info, &addr, &mask, &addr_len);
net/netlabel/netlabel_unlabeled.c-	if (ret_val != 0)
net/netlabel/netlabel_unlabeled.c-		return ret_val;
net/netlabel/netlabel_unlabeled.c:	ret_val = security_secctx_to_secid(
net/netlabel/netlabel_unlabeled.c-		                  nla_data(info->attrs[NLBL_UNLABEL_A_SECCTX]),
net/netlabel/netlabel_unlabeled.c-				  nla_len(info->attrs[NLBL_UNLABEL_A_SECCTX]),
net/netlabel/netlabel_unlabeled.c-				  &secid);
--
kernel/cred.c-	u32 secid;
kernel/cred.c-	int ret;
kernel/cred.c-
kernel/cred.c:	ret = security_secctx_to_secid(secctx, strlen(secctx), &secid);
kernel/cred.c-	if (ret < 0)
kernel/cred.c-		return ret;
kernel/cred.c-
--
net/netfilter/nft_meta.c-	u32 tmp_secid = 0;
net/netfilter/nft_meta.c-	int err;
net/netfilter/nft_meta.c-
net/netfilter/nft_meta.c:	err = security_secctx_to_secid(priv->ctx, strlen(priv->ctx), &tmp_secid);
net/netfilter/nft_meta.c-	if (err)
net/netfilter/nft_meta.c-		return err;
net/netfilter/nft_meta.c-
--
net/netfilter/xt_SECMARK.c-	info->secctx[SECMARK_SECCTX_MAX - 1] = '\0';
net/netfilter/xt_SECMARK.c-	info->secid = 0;
net/netfilter/xt_SECMARK.c-
net/netfilter/xt_SECMARK.c:	err = security_secctx_to_secid(info->secctx, strlen(info->secctx),
net/netfilter/xt_SECMARK.c-				       &info->secid);
net/netfilter/xt_SECMARK.c-	if (err) {
net/netfilter/xt_SECMARK.c-		if (err == -EINVAL)
--
fs/cachefiles/daemon.c-		return -EEXIST;
fs/cachefiles/daemon.c-	}
fs/cachefiles/daemon.c-
fs/cachefiles/daemon.c:	err = security_secctx_to_secid(args, strlen(args), &cache->secid);
fs/cachefiles/daemon.c-	if (err)
fs/cachefiles/daemon.c-		return err;
fs/cachefiles/daemon.c-
```

---

# security hook: security_secid_to_secctx

net/netlabel/netlabel_unlabeled.c:4
net/netlabel/netlabel_user.c:1
net/ipv4/ip_sockglue.c:1
kernel/auditsc.c:3
net/netfilter/nfnetlink_queue.c:1
net/netfilter/nf_conntrack_standalone.c:1
net/netfilter/nf_conntrack_netlink.c:2
kernel/audit.c:2
drivers/android/binder.c:1

```shell
net/ipv4/ip_sockglue.c-	if (err)
net/ipv4/ip_sockglue.c-		return;
net/ipv4/ip_sockglue.c-
net/ipv4/ip_sockglue.c:	err = security_secid_to_secctx(secid, &secdata, &seclen);
net/ipv4/ip_sockglue.c-	if (err)
net/ipv4/ip_sockglue.c-		return;
net/ipv4/ip_sockglue.c-
--
kernel/auditsc.c-			 from_kuid(&init_user_ns, auid),
kernel/auditsc.c-			 from_kuid(&init_user_ns, uid), sessionid);
kernel/auditsc.c-	if (sid) {
kernel/auditsc.c:		if (security_secid_to_secctx(sid, &ctx, &len)) {
kernel/auditsc.c-			audit_log_format(ab, " obj=(none)");
kernel/auditsc.c-			rc = 1;
kernel/auditsc.c-		} else {
--
kernel/auditsc.c-			char *ctx = NULL;
kernel/auditsc.c-			u32 len;
kernel/auditsc.c-
kernel/auditsc.c:			if (security_secid_to_secctx(osid, &ctx, &len)) {
kernel/auditsc.c-				audit_log_format(ab, " osid=%u", osid);
kernel/auditsc.c-				*call_panic = 1;
kernel/auditsc.c-			} else {
--
kernel/auditsc.c-		char *ctx = NULL;
kernel/auditsc.c-		u32 len;
kernel/auditsc.c-
kernel/auditsc.c:		if (security_secid_to_secctx(
kernel/auditsc.c-			n->osid, &ctx, &len)) {
kernel/auditsc.c-			audit_log_format(ab, " osid=%u", n->osid);
kernel/auditsc.c-			if (call_panic)
--
net/netfilter/nfnetlink_queue.c-	read_lock_bh(&skb->sk->sk_callback_lock);
net/netfilter/nfnetlink_queue.c-
net/netfilter/nfnetlink_queue.c-	if (skb->secmark)
net/netfilter/nfnetlink_queue.c:		security_secid_to_secctx(skb->secmark, secdata, &seclen);
net/netfilter/nfnetlink_queue.c-
net/netfilter/nfnetlink_queue.c-	read_unlock_bh(&skb->sk->sk_callback_lock);
net/netfilter/nfnetlink_queue.c-#endif
--
kernel/audit.c-	case AUDIT_SIGNAL_INFO:
kernel/audit.c-		len = 0;
kernel/audit.c-		if (audit_sig_sid) {
kernel/audit.c:			err = security_secid_to_secctx(audit_sig_sid, &ctx, &len);
kernel/audit.c-			if (err)
kernel/audit.c-				return err;
kernel/audit.c-		}
--
kernel/audit.c-	if (!sid)
kernel/audit.c-		return 0;
kernel/audit.c-
kernel/audit.c:	error = security_secid_to_secctx(sid, &ctx, &len);
kernel/audit.c-	if (error) {
kernel/audit.c-		if (error != -EINVAL)
kernel/audit.c-			goto error_path;
--
net/netfilter/nf_conntrack_standalone.c-	u32 len;
net/netfilter/nf_conntrack_standalone.c-	char *secctx;
net/netfilter/nf_conntrack_standalone.c-
net/netfilter/nf_conntrack_standalone.c:	ret = security_secid_to_secctx(ct->secmark, &secctx, &len);
net/netfilter/nf_conntrack_standalone.c-	if (ret)
net/netfilter/nf_conntrack_standalone.c-		return;
net/netfilter/nf_conntrack_standalone.c-
--
net/netfilter/nf_conntrack_netlink.c-	int len, ret;
net/netfilter/nf_conntrack_netlink.c-	char *secctx;
net/netfilter/nf_conntrack_netlink.c-
net/netfilter/nf_conntrack_netlink.c:	ret = security_secid_to_secctx(ct->secmark, &secctx, &len);
net/netfilter/nf_conntrack_netlink.c-	if (ret)
net/netfilter/nf_conntrack_netlink.c-		return 0;
net/netfilter/nf_conntrack_netlink.c-
--
net/netfilter/nf_conntrack_netlink.c-#ifdef CONFIG_NF_CONNTRACK_SECMARK
net/netfilter/nf_conntrack_netlink.c-	int len, ret;
net/netfilter/nf_conntrack_netlink.c-
net/netfilter/nf_conntrack_netlink.c:	ret = security_secid_to_secctx(ct->secmark, NULL, &len);
net/netfilter/nf_conntrack_netlink.c-	if (ret)
net/netfilter/nf_conntrack_netlink.c-		return 0;
net/netfilter/nf_conntrack_netlink.c-
--
net/netlabel/netlabel_unlabeled.c-unlhsh_add_return:
net/netlabel/netlabel_unlabeled.c-	rcu_read_unlock();
net/netlabel/netlabel_unlabeled.c-	if (audit_buf != NULL) {
net/netlabel/netlabel_unlabeled.c:		if (security_secid_to_secctx(secid,
net/netlabel/netlabel_unlabeled.c-					     &secctx,
net/netlabel/netlabel_unlabeled.c-					     &secctx_len) == 0) {
net/netlabel/netlabel_unlabeled.c-			audit_log_format(audit_buf, " sec_obj=%s", secctx);
--
net/netlabel/netlabel_unlabeled.c-					  addr->s_addr, mask->s_addr);
net/netlabel/netlabel_unlabeled.c-		dev_put(dev);
net/netlabel/netlabel_unlabeled.c-		if (entry != NULL &&
net/netlabel/netlabel_unlabeled.c:		    security_secid_to_secctx(entry->secid,
net/netlabel/netlabel_unlabeled.c-					     &secctx, &secctx_len) == 0) {
net/netlabel/netlabel_unlabeled.c-			audit_log_format(audit_buf, " sec_obj=%s", secctx);
net/netlabel/netlabel_unlabeled.c-			security_release_secctx(secctx, secctx_len);
--
net/netlabel/netlabel_unlabeled.c-					  addr, mask);
net/netlabel/netlabel_unlabeled.c-		dev_put(dev);
net/netlabel/netlabel_unlabeled.c-		if (entry != NULL &&
net/netlabel/netlabel_unlabeled.c:		    security_secid_to_secctx(entry->secid,
net/netlabel/netlabel_unlabeled.c-					     &secctx, &secctx_len) == 0) {
net/netlabel/netlabel_unlabeled.c-			audit_log_format(audit_buf, " sec_obj=%s", secctx);
net/netlabel/netlabel_unlabeled.c-			security_release_secctx(secctx, secctx_len);
--
net/netlabel/netlabel_unlabeled.c-		secid = addr6->secid;
net/netlabel/netlabel_unlabeled.c-	}
net/netlabel/netlabel_unlabeled.c-
net/netlabel/netlabel_unlabeled.c:	ret_val = security_secid_to_secctx(secid, &secctx, &secctx_len);
net/netlabel/netlabel_unlabeled.c-	if (ret_val != 0)
net/netlabel/netlabel_unlabeled.c-		goto list_cb_failure;
net/netlabel/netlabel_unlabeled.c-	ret_val = nla_put(cb_arg->skb,
--
net/netlabel/netlabel_user.c-			 audit_info->sessionid);
net/netlabel/netlabel_user.c-
net/netlabel/netlabel_user.c-	if (audit_info->secid != 0 &&
net/netlabel/netlabel_user.c:	    security_secid_to_secctx(audit_info->secid,
net/netlabel/netlabel_user.c-				     &secctx,
net/netlabel/netlabel_user.c-				     &secctx_len) == 0) {
net/netlabel/netlabel_user.c-		audit_log_format(audit_buf, " subj=%s", secctx);
--
drivers/android/binder.c-		size_t added_size;
drivers/android/binder.c-
drivers/android/binder.c-		security_cred_getsecid(proc->cred, &secid);
drivers/android/binder.c:		ret = security_secid_to_secctx(secid, &secctx, &secctx_sz);
drivers/android/binder.c-		if (ret) {
drivers/android/binder.c-			binder_txn_error("%d:%d failed to get security context\n",
drivers/android/binder.c-				thread->pid, proc->pid);
```

---

# security hook: security_secmark_refcount_dec

net/netfilter/nft_meta.c:1
net/netfilter/xt_SECMARK.c:1

```shell
net/netfilter/nft_meta.c-{
net/netfilter/nft_meta.c-	struct nft_secmark *priv = nft_obj_data(obj);
net/netfilter/nft_meta.c-
net/netfilter/nft_meta.c:	security_secmark_refcount_dec();
net/netfilter/nft_meta.c-
net/netfilter/nft_meta.c-	kfree(priv->ctx);
net/netfilter/nft_meta.c-}
--
net/netfilter/xt_SECMARK.c-{
net/netfilter/xt_SECMARK.c-	switch (mode) {
net/netfilter/xt_SECMARK.c-	case SECMARK_MODE_SEL:
net/netfilter/xt_SECMARK.c:		security_secmark_refcount_dec();
net/netfilter/xt_SECMARK.c-	}
net/netfilter/xt_SECMARK.c-}
net/netfilter/xt_SECMARK.c-
```

---

# security hook: security_secmark_refcount_inc

net/netfilter/nft_meta.c:1
net/netfilter/xt_SECMARK.c:1

```shell
net/netfilter/nft_meta.c-		return err;
net/netfilter/nft_meta.c-	}
net/netfilter/nft_meta.c-
net/netfilter/nft_meta.c:	security_secmark_refcount_inc();
net/netfilter/nft_meta.c-
net/netfilter/nft_meta.c-	return 0;
net/netfilter/nft_meta.c-}
--
net/netfilter/xt_SECMARK.c-		return err;
net/netfilter/xt_SECMARK.c-	}
net/netfilter/xt_SECMARK.c-
net/netfilter/xt_SECMARK.c:	security_secmark_refcount_inc();
net/netfilter/xt_SECMARK.c-	return 0;
net/netfilter/xt_SECMARK.c-}
net/netfilter/xt_SECMARK.c-
```

---

# security hook: security_secmark_relabel_packet

net/netfilter/nft_meta.c:1
net/netfilter/xt_SECMARK.c:1

```shell
net/netfilter/nft_meta.c-	if (!tmp_secid)
net/netfilter/nft_meta.c-		return -ENOENT;
net/netfilter/nft_meta.c-
net/netfilter/nft_meta.c:	err = security_secmark_relabel_packet(tmp_secid);
net/netfilter/nft_meta.c-	if (err)
net/netfilter/nft_meta.c-		return err;
net/netfilter/nft_meta.c-
--
net/netfilter/xt_SECMARK.c-		return -ENOENT;
net/netfilter/xt_SECMARK.c-	}
net/netfilter/xt_SECMARK.c-
net/netfilter/xt_SECMARK.c:	err = security_secmark_relabel_packet(info->secid);
net/netfilter/xt_SECMARK.c-	if (err) {
net/netfilter/xt_SECMARK.c-		pr_info_ratelimited("unable to obtain relabeling permission\n");
net/netfilter/xt_SECMARK.c-		return err;
```

---

# security hook: security_sem_alloc

ipc/sem.c:1

```shell
ipc/sem.c-	sma->sem_perm.key = key;
ipc/sem.c-
ipc/sem.c-	sma->sem_perm.security = NULL;
ipc/sem.c:	retval = security_sem_alloc(&sma->sem_perm);
ipc/sem.c-	if (retval) {
ipc/sem.c-		kvfree(sma);
ipc/sem.c-		return retval;
```

---

# security hook: security_sem_associate

ipc/util.h:1
ipc/sem.c:1

```shell
ipc/util.h- *      . routine to call to create a new ipc object. Can be one of newque,
ipc/util.h- *        newary, newseg
ipc/util.h- *      . routine to call to check permissions for a new ipc object.
ipc/util.h: *        Can be one of security_msg_associate, security_sem_associate,
ipc/util.h- *        security_shm_associate
ipc/util.h- *      . routine to call for an extra check if needed
ipc/util.h- */
--
ipc/sem.c-	struct ipc_namespace *ns;
ipc/sem.c-	static const struct ipc_ops sem_ops = {
ipc/sem.c-		.getnew = newary,
ipc/sem.c:		.associate = security_sem_associate,
ipc/sem.c-		.more_checks = sem_more_checks,
ipc/sem.c-	};
ipc/sem.c-	struct ipc_params sem_params;
```

---

# security hook: security_sem_free

ipc/sem.c:1

```shell
ipc/sem.c-	struct kern_ipc_perm *p = container_of(head, struct kern_ipc_perm, rcu);
ipc/sem.c-	struct sem_array *sma = container_of(p, struct sem_array, sem_perm);
ipc/sem.c-
ipc/sem.c:	security_sem_free(&sma->sem_perm);
ipc/sem.c-	kvfree(sma);
ipc/sem.c-}
ipc/sem.c-
```

---

# security hook: security_sem_semctl

ipc/sem.c:5

```shell
ipc/sem.c-			goto out_unlock;
ipc/sem.c-	}
ipc/sem.c-
ipc/sem.c:	err = security_sem_semctl(&sma->sem_perm, cmd);
ipc/sem.c-	if (err)
ipc/sem.c-		goto out_unlock;
ipc/sem.c-
--
ipc/sem.c-	int max_idx;
ipc/sem.c-	int err;
ipc/sem.c-
ipc/sem.c:	err = security_sem_semctl(NULL, cmd);
ipc/sem.c-	if (err)
ipc/sem.c-		return err;
ipc/sem.c-
--
ipc/sem.c-		return -EACCES;
ipc/sem.c-	}
ipc/sem.c-
ipc/sem.c:	err = security_sem_semctl(&sma->sem_perm, SETVAL);
ipc/sem.c-	if (err) {
ipc/sem.c-		rcu_read_unlock();
ipc/sem.c-		return -EACCES;
--
ipc/sem.c-	if (ipcperms(ns, &sma->sem_perm, cmd == SETALL ? S_IWUGO : S_IRUGO))
ipc/sem.c-		goto out_rcu_wakeup;
ipc/sem.c-
ipc/sem.c:	err = security_sem_semctl(&sma->sem_perm, cmd);
ipc/sem.c-	if (err)
ipc/sem.c-		goto out_rcu_wakeup;
ipc/sem.c-
--
ipc/sem.c-
ipc/sem.c-	sma = container_of(ipcp, struct sem_array, sem_perm);
ipc/sem.c-
ipc/sem.c:	err = security_sem_semctl(&sma->sem_perm, cmd);
ipc/sem.c-	if (err)
ipc/sem.c-		goto out_unlock1;
ipc/sem.c-
```

---

# security hook: security_sem_semop

ipc/sem.c:1

```shell
ipc/sem.c-		goto out;
ipc/sem.c-	}
ipc/sem.c-
ipc/sem.c:	error = security_sem_semop(&sma->sem_perm, sops, nsops, alter);
ipc/sem.c-	if (error) {
ipc/sem.c-		rcu_read_unlock();
ipc/sem.c-		goto out;
```

---

# security hook: security_setprocattr

fs/proc/base.c:1

```shell
fs/proc/base.c-	if (rv < 0)
fs/proc/base.c-		goto out_free;
fs/proc/base.c-
fs/proc/base.c:	rv = security_setprocattr(PROC_I(inode)->op.lsm,
fs/proc/base.c-				  file->f_path.dentry->d_name.name, page,
fs/proc/base.c-				  count);
fs/proc/base.c-	mutex_unlock(&current->signal->cred_guard_mutex);
```

---

# security hook: security_settime64

kernel/time/time.c:3

```shell
kernel/time/time.c-
kernel/time/time.c-	tv.tv_nsec = 0;
kernel/time/time.c-
kernel/time/time.c:	err = security_settime64(&tv, NULL);
kernel/time/time.c-	if (err)
kernel/time/time.c-		return err;
kernel/time/time.c-
--
kernel/time/time.c-
kernel/time/time.c-	tv.tv_nsec = 0;
kernel/time/time.c-
kernel/time/time.c:	err = security_settime64(&tv, NULL);
kernel/time/time.c-	if (err)
kernel/time/time.c-		return err;
kernel/time/time.c-
--
kernel/time/time.c-	if (tv && !timespec64_valid_settod(tv))
kernel/time/time.c-		return -EINVAL;
kernel/time/time.c-
kernel/time/time.c:	error = security_settime64(tv, tz);
kernel/time/time.c-	if (error)
kernel/time/time.c-		return error;
kernel/time/time.c-
```

---

# security hook: security_shm_alloc

ipc/shm.c:1

```shell
ipc/shm.c-	shp->mlock_ucounts = NULL;
ipc/shm.c-
ipc/shm.c-	shp->shm_perm.security = NULL;
ipc/shm.c:	error = security_shm_alloc(&shp->shm_perm);
ipc/shm.c-	if (error) {
ipc/shm.c-		kfree(shp);
ipc/shm.c-		return error;
```

---

# security hook: security_shm_associate

ipc/util.h:1
ipc/shm.c:1

```shell
ipc/util.h- *        newary, newseg
ipc/util.h- *      . routine to call to check permissions for a new ipc object.
ipc/util.h- *        Can be one of security_msg_associate, security_sem_associate,
ipc/util.h: *        security_shm_associate
ipc/util.h- *      . routine to call for an extra check if needed
ipc/util.h- */
ipc/util.h-struct ipc_ops {
--
ipc/shm.c-	struct ipc_namespace *ns;
ipc/shm.c-	static const struct ipc_ops shm_ops = {
ipc/shm.c-		.getnew = newseg,
ipc/shm.c:		.associate = security_shm_associate,
ipc/shm.c-		.more_checks = shm_more_checks,
ipc/shm.c-	};
ipc/shm.c-	struct ipc_params shm_params;
```

---

# security hook: security_shm_free

ipc/shm.c:1

```shell
ipc/shm.c-							rcu);
ipc/shm.c-	struct shmid_kernel *shp = container_of(ptr, struct shmid_kernel,
ipc/shm.c-							shm_perm);
ipc/shm.c:	security_shm_free(&shp->shm_perm);
ipc/shm.c-	kfree(shp);
ipc/shm.c-}
ipc/shm.c-
```

---

# security hook: security_shm_shmat

ipc/shm.c:1

```shell
ipc/shm.c-	if (ipcperms(ns, &shp->shm_perm, acc_mode))
ipc/shm.c-		goto out_unlock;
ipc/shm.c-
ipc/shm.c:	err = security_shm_shmat(&shp->shm_perm, shmaddr, shmflg);
ipc/shm.c-	if (err)
ipc/shm.c-		goto out_unlock;
ipc/shm.c-
```

---

# security hook: security_shm_shmctl

ipc/shm.c:5

```shell
ipc/shm.c-
ipc/shm.c-	shp = container_of(ipcp, struct shmid_kernel, shm_perm);
ipc/shm.c-
ipc/shm.c:	err = security_shm_shmctl(&shp->shm_perm, cmd);
ipc/shm.c-	if (err)
ipc/shm.c-		goto out_unlock1;
ipc/shm.c-
--
ipc/shm.c-static int shmctl_ipc_info(struct ipc_namespace *ns,
ipc/shm.c-			   struct shminfo64 *shminfo)
ipc/shm.c-{
ipc/shm.c:	int err = security_shm_shmctl(NULL, IPC_INFO);
ipc/shm.c-	if (!err) {
ipc/shm.c-		memset(shminfo, 0, sizeof(*shminfo));
ipc/shm.c-		shminfo->shmmni = shminfo->shmseg = ns->shm_ctlmni;
--
ipc/shm.c-static int shmctl_shm_info(struct ipc_namespace *ns,
ipc/shm.c-			   struct shm_info *shm_info)
ipc/shm.c-{
ipc/shm.c:	int err = security_shm_shmctl(NULL, SHM_INFO);
ipc/shm.c-	if (!err) {
ipc/shm.c-		memset(shm_info, 0, sizeof(*shm_info));
ipc/shm.c-		down_read(&shm_ids(ns).rwsem);
--
ipc/shm.c-			goto out_unlock;
ipc/shm.c-	}
ipc/shm.c-
ipc/shm.c:	err = security_shm_shmctl(&shp->shm_perm, cmd);
ipc/shm.c-	if (err)
ipc/shm.c-		goto out_unlock;
ipc/shm.c-
--
ipc/shm.c-	}
ipc/shm.c-
ipc/shm.c-	audit_ipc_obj(&(shp->shm_perm));
ipc/shm.c:	err = security_shm_shmctl(&shp->shm_perm, cmd);
ipc/shm.c-	if (err)
ipc/shm.c-		goto out_unlock1;
ipc/shm.c-
```

---

# security hook: security_sk_alloc

net/core/sock.c:1

```shell
net/core/sock.c-		sk = kmalloc(prot->obj_size, priority);
net/core/sock.c-
net/core/sock.c-	if (sk != NULL) {
net/core/sock.c:		if (security_sk_alloc(sk, family, priority))
net/core/sock.c-			goto out_free;
net/core/sock.c-
net/core/sock.c-		if (!try_module_get(prot->owner))
```

---

# security hook: security_skb_classify_flow

net/ipv4/icmp.c:2
net/ipv4/ip_output.c:1
net/netfilter/nf_synproxy_core.c:1
net/dccp/ipv6.c:1
net/dccp/ipv4.c:1
net/ipv6/netfilter/nf_reject_ipv6.c:1
net/ipv6/icmp.c:2
net/ipv6/tcp_ipv6.c:1

```shell
net/ipv4/icmp.c-	fl4.flowi4_tos = RT_TOS(ip_hdr(skb)->tos);
net/ipv4/icmp.c-	fl4.flowi4_proto = IPPROTO_ICMP;
net/ipv4/icmp.c-	fl4.flowi4_oif = l3mdev_master_ifindex(skb->dev);
net/ipv4/icmp.c:	security_skb_classify_flow(skb, flowi4_to_flowi_common(&fl4));
net/ipv4/icmp.c-	rt = ip_route_output_key(net, &fl4);
net/ipv4/icmp.c-	if (IS_ERR(rt))
net/ipv4/icmp.c-		goto out_unlock;
--
net/ipv4/icmp.c-	route_lookup_dev = icmp_get_route_lookup_dev(skb_in);
net/ipv4/icmp.c-	fl4->flowi4_oif = l3mdev_master_ifindex(route_lookup_dev);
net/ipv4/icmp.c-
net/ipv4/icmp.c:	security_skb_classify_flow(skb_in, flowi4_to_flowi_common(fl4));
net/ipv4/icmp.c-	rt = ip_route_output_key_hash(net, fl4, skb_in);
net/ipv4/icmp.c-	if (IS_ERR(rt))
net/ipv4/icmp.c-		return rt;
--
net/ipv4/ip_output.c-			   daddr, saddr,
net/ipv4/ip_output.c-			   tcp_hdr(skb)->source, tcp_hdr(skb)->dest,
net/ipv4/ip_output.c-			   arg->uid);
net/ipv4/ip_output.c:	security_skb_classify_flow(skb, flowi4_to_flowi_common(&fl4));
net/ipv4/ip_output.c-	rt = ip_route_output_flow(net, &fl4, sk);
net/ipv4/ip_output.c-	if (IS_ERR(rt))
net/ipv4/ip_output.c-		return;
--
net/netfilter/nf_synproxy_core.c-	fl6.daddr = niph->daddr;
net/netfilter/nf_synproxy_core.c-	fl6.fl6_sport = nth->source;
net/netfilter/nf_synproxy_core.c-	fl6.fl6_dport = nth->dest;
net/netfilter/nf_synproxy_core.c:	security_skb_classify_flow((struct sk_buff *)skb,
net/netfilter/nf_synproxy_core.c-				   flowi6_to_flowi_common(&fl6));
net/netfilter/nf_synproxy_core.c-	err = nf_ip6_route(net, &dst, flowi6_to_flowi(&fl6), false);
net/netfilter/nf_synproxy_core.c-	if (err) {
--
net/dccp/ipv6.c-	fl6.flowi6_oif = inet6_iif(rxskb);
net/dccp/ipv6.c-	fl6.fl6_dport = dccp_hdr(skb)->dccph_dport;
net/dccp/ipv6.c-	fl6.fl6_sport = dccp_hdr(skb)->dccph_sport;
net/dccp/ipv6.c:	security_skb_classify_flow(rxskb, flowi6_to_flowi_common(&fl6));
net/dccp/ipv6.c-
net/dccp/ipv6.c-	/* sk = NULL, but it is safe for now. RST socket required. */
net/dccp/ipv6.c-	dst = ip6_dst_lookup_flow(sock_net(ctl_sk), ctl_sk, &fl6, NULL);
--
net/dccp/ipv4.c-		.fl4_dport = dccp_hdr(skb)->dccph_sport,
net/dccp/ipv4.c-	};
net/dccp/ipv4.c-
net/dccp/ipv4.c:	security_skb_classify_flow(skb, flowi4_to_flowi_common(&fl4));
net/dccp/ipv4.c-	rt = ip_route_output_flow(net, &fl4, sk);
net/dccp/ipv4.c-	if (IS_ERR(rt)) {
net/dccp/ipv4.c-		IP_INC_STATS(net, IPSTATS_MIB_OUTNOROUTES);
--
net/ipv6/netfilter/nf_reject_ipv6.c-
net/ipv6/netfilter/nf_reject_ipv6.c-	fl6.flowi6_oif = l3mdev_master_ifindex(skb_dst(oldskb)->dev);
net/ipv6/netfilter/nf_reject_ipv6.c-	fl6.flowi6_mark = IP6_REPLY_MARK(net, oldskb->mark);
net/ipv6/netfilter/nf_reject_ipv6.c:	security_skb_classify_flow(oldskb, flowi6_to_flowi_common(&fl6));
net/ipv6/netfilter/nf_reject_ipv6.c-	dst = ip6_route_output(net, NULL, &fl6);
net/ipv6/netfilter/nf_reject_ipv6.c-	if (dst->error) {
net/ipv6/netfilter/nf_reject_ipv6.c-		dst_release(dst);
--
net/ipv6/icmp.c-	fl6.fl6_icmp_code = code;
net/ipv6/icmp.c-	fl6.flowi6_uid = sock_net_uid(net, NULL);
net/ipv6/icmp.c-	fl6.mp_hash = rt6_multipath_hash(net, &fl6, skb, NULL);
net/ipv6/icmp.c:	security_skb_classify_flow(skb, flowi6_to_flowi_common(&fl6));
net/ipv6/icmp.c-
net/ipv6/icmp.c-	np = inet6_sk(sk);
net/ipv6/icmp.c-
--
net/ipv6/icmp.c-	fl6.fl6_icmp_type = type;
net/ipv6/icmp.c-	fl6.flowi6_mark = mark;
net/ipv6/icmp.c-	fl6.flowi6_uid = sock_net_uid(net, NULL);
net/ipv6/icmp.c:	security_skb_classify_flow(skb, flowi6_to_flowi_common(&fl6));
net/ipv6/icmp.c-
net/ipv6/icmp.c-	local_bh_disable();
net/ipv6/icmp.c-	sk = icmpv6_xmit_lock(net);
--
net/ipv6/tcp_ipv6.c-	fl6.fl6_dport = t1->dest;
net/ipv6/tcp_ipv6.c-	fl6.fl6_sport = t1->source;
net/ipv6/tcp_ipv6.c-	fl6.flowi6_uid = sock_net_uid(net, sk && sk_fullsock(sk) ? sk : NULL);
net/ipv6/tcp_ipv6.c:	security_skb_classify_flow(skb, flowi6_to_flowi_common(&fl6));
net/ipv6/tcp_ipv6.c-
net/ipv6/tcp_ipv6.c-	/* Pass a socket to ip6_dst_lookup either it is for RST
net/ipv6/tcp_ipv6.c-	 * Underlying function will use this to retrieve the network
```

---

# security hook: security_sk_classify_flow

net/ipv4/raw.c:1
net/ipv4/udp.c:1
net/ipv4/ping.c:1
net/dccp/ipv6.c:1
net/ipv6/inet6_connection_sock.c:1
net/ipv6/af_inet6.c:1
net/ipv6/ping.c:1
net/ipv6/icmp.c:1
net/ipv6/tcp_ipv6.c:1
net/ipv6/udp.c:1
net/ipv6/datagram.c:1
net/ipv6/raw.c:1
net/l2tp/l2tp_ip6.c:1
drivers/net/wireguard/socket.c:2
drivers/net/ppp/pptp.c:1

```shell
net/ipv4/raw.c-			goto done;
net/ipv4/raw.c-	}
net/ipv4/raw.c-
net/ipv4/raw.c:	security_sk_classify_flow(sk, flowi4_to_flowi_common(&fl4));
net/ipv4/raw.c-	rt = ip_route_output_flow(net, &fl4, sk);
net/ipv4/raw.c-	if (IS_ERR(rt)) {
net/ipv4/raw.c-		err = PTR_ERR(rt);
--
net/ipv4/udp.c-				   sk->sk_protocol, flow_flags, faddr, saddr,
net/ipv4/udp.c-				   dport, inet->inet_sport, sk->sk_uid);
net/ipv4/udp.c-
net/ipv4/udp.c:		security_sk_classify_flow(sk, flowi4_to_flowi_common(fl4));
net/ipv4/udp.c-		rt = ip_route_output_flow(net, fl4, sk);
net/ipv4/udp.c-		if (IS_ERR(rt)) {
net/ipv4/udp.c-			err = PTR_ERR(rt);
--
net/ipv4/ping.c-	fl4.fl4_icmp_type = user_icmph.type;
net/ipv4/ping.c-	fl4.fl4_icmp_code = user_icmph.code;
net/ipv4/ping.c-
net/ipv4/ping.c:	security_sk_classify_flow(sk, flowi4_to_flowi_common(&fl4));
net/ipv4/ping.c-	rt = ip_route_output_flow(net, &fl4, sk);
net/ipv4/ping.c-	if (IS_ERR(rt)) {
net/ipv4/ping.c-		err = PTR_ERR(rt);
--
net/dccp/ipv6.c-	fl6.flowi6_oif = sk->sk_bound_dev_if;
net/dccp/ipv6.c-	fl6.fl6_dport = usin->sin6_port;
net/dccp/ipv6.c-	fl6.fl6_sport = inet->inet_sport;
net/dccp/ipv6.c:	security_sk_classify_flow(sk, flowi6_to_flowi_common(&fl6));
net/dccp/ipv6.c-
net/dccp/ipv6.c-	opt = rcu_dereference_protected(np->opt, lockdep_sock_is_held(sk));
net/dccp/ipv6.c-	final_p = fl6_update_dst(&fl6, opt, &final);
--
net/l2tp/l2tp_ip6.c-	else if (!fl6.flowi6_oif)
net/l2tp/l2tp_ip6.c-		fl6.flowi6_oif = np->ucast_oif;
net/l2tp/l2tp_ip6.c-
net/l2tp/l2tp_ip6.c:	security_sk_classify_flow(sk, flowi6_to_flowi_common(&fl6));
net/l2tp/l2tp_ip6.c-
net/l2tp/l2tp_ip6.c-	if (ipc6.tclass < 0)
net/l2tp/l2tp_ip6.c-		ipc6.tclass = np->tclass;
--
net/ipv6/inet6_connection_sock.c-	fl6->fl6_sport = inet->inet_sport;
net/ipv6/inet6_connection_sock.c-	fl6->fl6_dport = inet->inet_dport;
net/ipv6/inet6_connection_sock.c-	fl6->flowi6_uid = sk->sk_uid;
net/ipv6/inet6_connection_sock.c:	security_sk_classify_flow(sk, flowi6_to_flowi_common(fl6));
net/ipv6/inet6_connection_sock.c-
net/ipv6/inet6_connection_sock.c-	rcu_read_lock();
net/ipv6/inet6_connection_sock.c-	final_p = fl6_update_dst(fl6, rcu_dereference(np->opt), &final);
--
net/ipv6/icmp.c-	fl6->fl6_icmp_type	= type;
net/ipv6/icmp.c-	fl6->fl6_icmp_code	= 0;
net/ipv6/icmp.c-	fl6->flowi6_oif		= oif;
net/ipv6/icmp.c:	security_sk_classify_flow(sk, flowi6_to_flowi_common(fl6));
net/ipv6/icmp.c-}
net/ipv6/icmp.c-
net/ipv6/icmp.c-int __init icmpv6_init(void)
--
net/ipv6/datagram.c-	}
net/ipv6/datagram.c-
net/ipv6/datagram.c-	fl6->flowi6_oif = oif;
net/ipv6/datagram.c:	security_sk_classify_flow(sk, flowi6_to_flowi_common(fl6));
net/ipv6/datagram.c-}
net/ipv6/datagram.c-
net/ipv6/datagram.c-int ip6_datagram_dst_update(struct sock *sk, bool fix_sk_saddr)
--
net/ipv6/raw.c-		fl6.flowi6_oif = np->mcast_oif;
net/ipv6/raw.c-	else if (!fl6.flowi6_oif)
net/ipv6/raw.c-		fl6.flowi6_oif = np->ucast_oif;
net/ipv6/raw.c:	security_sk_classify_flow(sk, flowi6_to_flowi_common(&fl6));
net/ipv6/raw.c-
net/ipv6/raw.c-	if (hdrincl)
net/ipv6/raw.c-		fl6.flowi6_flags |= FLOWI_FLAG_KNOWN_NH;
--
net/ipv6/tcp_ipv6.c-	opt = rcu_dereference_protected(np->opt, lockdep_sock_is_held(sk));
net/ipv6/tcp_ipv6.c-	final_p = fl6_update_dst(&fl6, opt, &final);
net/ipv6/tcp_ipv6.c-
net/ipv6/tcp_ipv6.c:	security_sk_classify_flow(sk, flowi6_to_flowi_common(&fl6));
net/ipv6/tcp_ipv6.c-
net/ipv6/tcp_ipv6.c-	dst = ip6_dst_lookup_flow(net, sk, &fl6, final_p);
net/ipv6/tcp_ipv6.c-	if (IS_ERR(dst)) {
--
net/ipv6/udp.c-	} else if (!fl6->flowi6_oif)
net/ipv6/udp.c-		fl6->flowi6_oif = np->ucast_oif;
net/ipv6/udp.c-
net/ipv6/udp.c:	security_sk_classify_flow(sk, flowi6_to_flowi_common(fl6));
net/ipv6/udp.c-
net/ipv6/udp.c-	if (ipc6.tclass < 0)
net/ipv6/udp.c-		ipc6.tclass = np->tclass;
--
net/ipv6/af_inet6.c-		fl6.fl6_dport = inet->inet_dport;
net/ipv6/af_inet6.c-		fl6.fl6_sport = inet->inet_sport;
net/ipv6/af_inet6.c-		fl6.flowi6_uid = sk->sk_uid;
net/ipv6/af_inet6.c:		security_sk_classify_flow(sk, flowi6_to_flowi_common(&fl6));
net/ipv6/af_inet6.c-
net/ipv6/af_inet6.c-		rcu_read_lock();
net/ipv6/af_inet6.c-		final_p = fl6_update_dst(&fl6, rcu_dereference(np->opt),
--
net/ipv6/ping.c-	fl6.flowi6_uid = sk->sk_uid;
net/ipv6/ping.c-	fl6.fl6_icmp_type = user_icmph.icmp6_type;
net/ipv6/ping.c-	fl6.fl6_icmp_code = user_icmph.icmp6_code;
net/ipv6/ping.c:	security_sk_classify_flow(sk, flowi6_to_flowi_common(&fl6));
net/ipv6/ping.c-
net/ipv6/ping.c-	fl6.flowlabel = ip6_make_flowinfo(ipc6.tclass, fl6.flowlabel);
net/ipv6/ping.c-
--
drivers/net/wireguard/socket.c-		rt = dst_cache_get_ip4(cache, &fl.saddr);
drivers/net/wireguard/socket.c-
drivers/net/wireguard/socket.c-	if (!rt) {
drivers/net/wireguard/socket.c:		security_sk_classify_flow(sock, flowi4_to_flowi_common(&fl));
drivers/net/wireguard/socket.c-		if (unlikely(!inet_confirm_addr(sock_net(sock), NULL, 0,
drivers/net/wireguard/socket.c-						fl.saddr, RT_SCOPE_HOST))) {
drivers/net/wireguard/socket.c-			endpoint->src4.s_addr = 0;
--
drivers/net/wireguard/socket.c-		dst = dst_cache_get_ip6(cache, &fl.saddr);
drivers/net/wireguard/socket.c-
drivers/net/wireguard/socket.c-	if (!dst) {
drivers/net/wireguard/socket.c:		security_sk_classify_flow(sock, flowi6_to_flowi_common(&fl));
drivers/net/wireguard/socket.c-		if (unlikely(!ipv6_addr_any(&fl.saddr) &&
drivers/net/wireguard/socket.c-			     !ipv6_chk_addr(sock_net(sock), &fl.saddr, NULL, 0))) {
drivers/net/wireguard/socket.c-			endpoint->src6 = fl.saddr = in6addr_any;
--
drivers/net/ppp/pptp.c-			   po->proto.pptp.dst_addr.sin_addr.s_addr,
drivers/net/ppp/pptp.c-			   po->proto.pptp.src_addr.sin_addr.s_addr,
drivers/net/ppp/pptp.c-			   0, 0, sock_net_uid(net, sk));
drivers/net/ppp/pptp.c:	security_sk_classify_flow(sk, flowi4_to_flowi_common(fl4));
drivers/net/ppp/pptp.c-
drivers/net/ppp/pptp.c-	return ip_route_output_flow(net, fl4, sk);
drivers/net/ppp/pptp.c-}
```

---

# security hook: security_sk_clone

crypto/af_alg.c:1
net/iucv/af_iucv.c:1
net/bluetooth/sco.c:1
net/bluetooth/l2cap_sock.c:1
net/bluetooth/iso.c:1
net/bluetooth/rfcomm/sock.c:1
net/tipc/socket.c:1
net/core/sock.c:1
net/vmw_vsock/af_vsock.c:1

```shell
crypto/af_alg.c-
crypto/af_alg.c-	sock_init_data(newsock, sk2);
crypto/af_alg.c-	security_sock_graft(sk2, newsock);
crypto/af_alg.c:	security_sk_clone(sk, sk2);
crypto/af_alg.c-
crypto/af_alg.c-	/*
crypto/af_alg.c-	 * newsock->ops assigned here to allow type->accept call to override
--
net/iucv/af_iucv.c-{
net/iucv/af_iucv.c-	if (parent) {
net/iucv/af_iucv.c-		sk->sk_type = parent->sk_type;
net/iucv/af_iucv.c:		security_sk_clone(parent, sk);
net/iucv/af_iucv.c-	}
net/iucv/af_iucv.c-}
net/iucv/af_iucv.c-
--
net/core/sock.c-
net/core/sock.c-#ifdef CONFIG_SECURITY_NETWORK
net/core/sock.c-	nsk->sk_security = sptr;
net/core/sock.c:	security_sk_clone(osk, nsk);
net/core/sock.c-#endif
net/core/sock.c-}
net/core/sock.c-
--
net/tipc/socket.c-	res = tipc_sk_create(sock_net(sock->sk), new_sock, 0, kern);
net/tipc/socket.c-	if (res)
net/tipc/socket.c-		goto exit;
net/tipc/socket.c:	security_sk_clone(sock->sk, new_sock->sk);
net/tipc/socket.c-
net/tipc/socket.c-	new_sk = new_sock->sk;
net/tipc/socket.c-	new_tsock = tipc_sk(new_sk);
--
net/vmw_vsock/af_vsock.c-		vsk->buffer_size = psk->buffer_size;
net/vmw_vsock/af_vsock.c-		vsk->buffer_min_size = psk->buffer_min_size;
net/vmw_vsock/af_vsock.c-		vsk->buffer_max_size = psk->buffer_max_size;
net/vmw_vsock/af_vsock.c:		security_sk_clone(parent, sk);
net/vmw_vsock/af_vsock.c-	} else {
net/vmw_vsock/af_vsock.c-		vsk->trusted = ns_capable_noaudit(&init_user_ns, CAP_NET_ADMIN);
net/vmw_vsock/af_vsock.c-		vsk->owner = get_current_cred();
--
net/bluetooth/sco.c-	if (parent) {
net/bluetooth/sco.c-		sk->sk_type = parent->sk_type;
net/bluetooth/sco.c-		bt_sk(sk)->flags = bt_sk(parent)->flags;
net/bluetooth/sco.c:		security_sk_clone(parent, sk);
net/bluetooth/sco.c-	}
net/bluetooth/sco.c-}
net/bluetooth/sco.c-
--
net/bluetooth/l2cap_sock.c-			chan->dcid = pchan->scid;
net/bluetooth/l2cap_sock.c-		}
net/bluetooth/l2cap_sock.c-
net/bluetooth/l2cap_sock.c:		security_sk_clone(parent, sk);
net/bluetooth/l2cap_sock.c-	} else {
net/bluetooth/l2cap_sock.c-		switch (sk->sk_type) {
net/bluetooth/l2cap_sock.c-		case SOCK_RAW:
--
net/bluetooth/iso.c-	if (parent) {
net/bluetooth/iso.c-		sk->sk_type = parent->sk_type;
net/bluetooth/iso.c-		bt_sk(sk)->flags = bt_sk(parent)->flags;
net/bluetooth/iso.c:		security_sk_clone(parent, sk);
net/bluetooth/iso.c-	}
net/bluetooth/iso.c-}
net/bluetooth/iso.c-
--
net/bluetooth/rfcomm/sock.c-		pi->sec_level = rfcomm_pi(parent)->sec_level;
net/bluetooth/rfcomm/sock.c-		pi->role_switch = rfcomm_pi(parent)->role_switch;
net/bluetooth/rfcomm/sock.c-
net/bluetooth/rfcomm/sock.c:		security_sk_clone(parent, sk);
net/bluetooth/rfcomm/sock.c-	} else {
net/bluetooth/rfcomm/sock.c-		pi->dlc->defer_setup = 0;
net/bluetooth/rfcomm/sock.c-
```

---

# security hook: security_sk_free

net/core/sock.c:2

```shell
net/core/sock.c-	return sk;
net/core/sock.c-
net/core/sock.c-out_free_sec:
net/core/sock.c:	security_sk_free(sk);
net/core/sock.c-out_free:
net/core/sock.c-	if (slab != NULL)
net/core/sock.c-		kmem_cache_free(slab, sk);
--
net/core/sock.c-
net/core/sock.c-	cgroup_sk_free(&sk->sk_cgrp_data);
net/core/sock.c-	mem_cgroup_sk_free(sk);
net/core/sock.c:	security_sk_free(sk);
net/core/sock.c-	if (slab != NULL)
net/core/sock.c-		kmem_cache_free(slab, sk);
net/core/sock.c-	else
```

---

# security hook: security_socket_accept

net/socket.c:1

```shell
net/socket.c-	if (IS_ERR(newfile))
net/socket.c-		return newfile;
net/socket.c-
net/socket.c:	err = security_socket_accept(sock, newsock);
net/socket.c-	if (err)
net/socket.c-		goto out_fd;
net/socket.c-
```

---

# security hook: security_socket_bind

net/socket.c:1

```shell
net/socket.c-	if (sock) {
net/socket.c-		err = move_addr_to_kernel(umyaddr, addrlen, &address);
net/socket.c-		if (!err) {
net/socket.c:			err = security_socket_bind(sock,
net/socket.c-						   (struct sockaddr *)&address,
net/socket.c-						   addrlen);
net/socket.c-			if (!err)
```

---

# security hook: security_socket_connect

net/socket.c:1

```shell
net/socket.c-	}
net/socket.c-
net/socket.c-	err =
net/socket.c:	    security_socket_connect(sock, (struct sockaddr *)address, addrlen);
net/socket.c-	if (err)
net/socket.c-		goto out;
net/socket.c-
```

---

# security hook: security_socket_create

net/socket.c:2

```shell
net/socket.c-	int err;
net/socket.c-	struct socket *sock = NULL;
net/socket.c-
net/socket.c:	err = security_socket_create(family, type, protocol, 1);
net/socket.c-	if (err)
net/socket.c-		goto out;
net/socket.c-
--
net/socket.c-		family = PF_PACKET;
net/socket.c-	}
net/socket.c-
net/socket.c:	err = security_socket_create(family, type, protocol, kern);
net/socket.c-	if (err)
net/socket.c-		return err;
net/socket.c-
```

---

# security hook: security_socket_getpeername

net/socket.c:1

```shell
net/socket.c-	if (sock != NULL) {
net/socket.c-		const struct proto_ops *ops = READ_ONCE(sock->ops);
net/socket.c-
net/socket.c:		err = security_socket_getpeername(sock);
net/socket.c-		if (err) {
net/socket.c-			fput_light(sock->file, fput_needed);
net/socket.c-			return err;
```

---

# security hook: security_socket_getpeersec_dgram

net/ipv4/ip_sockglue.c:1

```shell
net/ipv4/ip_sockglue.c-	u32 seclen, secid;
net/ipv4/ip_sockglue.c-	int err;
net/ipv4/ip_sockglue.c-
net/ipv4/ip_sockglue.c:	err = security_socket_getpeersec_dgram(NULL, skb, &secid);
net/ipv4/ip_sockglue.c-	if (err)
net/ipv4/ip_sockglue.c-		return;
net/ipv4/ip_sockglue.c-
```

---

# security hook: security_socket_getpeersec_stream

net/core/sock.c:1

```shell
net/core/sock.c-		break;
net/core/sock.c-
net/core/sock.c-	case SO_PEERSEC:
net/core/sock.c:		return security_socket_getpeersec_stream(sock,
net/core/sock.c-							 optval, optlen, len);
net/core/sock.c-
net/core/sock.c-	case SO_MARK:
```

---

# security hook: security_socket_getsockname

net/socket.c:1

```shell
net/socket.c-	if (!sock)
net/socket.c-		goto out;
net/socket.c-
net/socket.c:	err = security_socket_getsockname(sock);
net/socket.c-	if (err)
net/socket.c-		goto out_put;
net/socket.c-
```

---

# security hook: security_socket_getsockopt

net/socket.c:1

```shell
net/socket.c-	const struct proto_ops *ops;
net/socket.c-	int err;
net/socket.c-
net/socket.c:	err = security_socket_getsockopt(sock, level, optname);
net/socket.c-	if (err)
net/socket.c-		return err;
net/socket.c-
```

---

# security hook: security_socket_listen

net/socket.c:1

```shell
net/socket.c-		if ((unsigned int)backlog > somaxconn)
net/socket.c-			backlog = somaxconn;
net/socket.c-
net/socket.c:		err = security_socket_listen(sock, backlog);
net/socket.c-		if (!err)
net/socket.c-			err = READ_ONCE(sock->ops)->listen(sock, backlog);
net/socket.c-
```

---

# security hook: security_socket_post_create

net/socket.c:2

```shell
net/socket.c-	}
net/socket.c-
net/socket.c-	sock->type = type;
net/socket.c:	err = security_socket_post_create(sock, family, type, protocol, 1);
net/socket.c-	if (err)
net/socket.c-		goto out_release;
net/socket.c-
--
net/socket.c-	 * module can have its refcnt decremented
net/socket.c-	 */
net/socket.c-	module_put(pf->owner);
net/socket.c:	err = security_socket_post_create(sock, family, type, protocol, kern);
net/socket.c-	if (err)
net/socket.c-		goto out_sock_release;
net/socket.c-	*res = sock;
```

---

# security hook: security_socket_recvmsg

net/socket.c:1

```shell
net/socket.c- */
net/socket.c-int sock_recvmsg(struct socket *sock, struct msghdr *msg, int flags)
net/socket.c-{
net/socket.c:	int err = security_socket_recvmsg(sock, msg, msg_data_left(msg), flags);
net/socket.c-
net/socket.c-	return err ?: sock_recvmsg_nosec(sock, msg, flags);
net/socket.c-}
```

---

# security hook: security_socket_sendmsg

net/socket.c:1

```shell
net/socket.c-
net/socket.c-static int __sock_sendmsg(struct socket *sock, struct msghdr *msg)
net/socket.c-{
net/socket.c:	int err = security_socket_sendmsg(sock, msg,
net/socket.c-					  msg_data_left(msg));
net/socket.c-
net/socket.c-	return err ?: sock_sendmsg_nosec(sock, msg);
```

---

# security hook: security_socket_setsockopt

net/socket.c:1

```shell
net/socket.c-	if (optlen < 0)
net/socket.c-		return -EINVAL;
net/socket.c-
net/socket.c:	err = security_socket_setsockopt(sock, level, optname);
net/socket.c-	if (err)
net/socket.c-		goto out_put;
net/socket.c-
```

---

# security hook: security_socket_shutdown

net/socket.c:1

```shell
net/socket.c-{
net/socket.c-	int err;
net/socket.c-
net/socket.c:	err = security_socket_shutdown(sock, how);
net/socket.c-	if (!err)
net/socket.c-		err = READ_ONCE(sock->ops)->shutdown(sock, how);
net/socket.c-
```

---

# security hook: security_socket_socketpair

net/socket.c:1

```shell
net/socket.c-		goto out;
net/socket.c-	}
net/socket.c-
net/socket.c:	err = security_socket_socketpair(sock1, sock2);
net/socket.c-	if (unlikely(err)) {
net/socket.c-		sock_release(sock2);
net/socket.c-		sock_release(sock1);
```

---

# security hook: security_sock_graft

crypto/af_alg.c:1

```shell
crypto/af_alg.c-		goto unlock;
crypto/af_alg.c-
crypto/af_alg.c-	sock_init_data(newsock, sk2);
crypto/af_alg.c:	security_sock_graft(sk2, newsock);
crypto/af_alg.c-	security_sk_clone(sk, sk2);
crypto/af_alg.c-
crypto/af_alg.c-	/*
```

---

# security hook: security_sock_rcv_skb

net/core/filter.c:1

```shell
net/core/filter.c-	if (err)
net/core/filter.c-		return err;
net/core/filter.c-
net/core/filter.c:	err = security_sock_rcv_skb(sk, skb);
net/core/filter.c-	if (err)
net/core/filter.c-		return err;
net/core/filter.c-
```

---

# security hook: security_syslog

kernel/printk/printk.c:1

```shell
kernel/printk/printk.c-		return -EPERM;
kernel/printk/printk.c-	}
kernel/printk/printk.c-ok:
kernel/printk/printk.c:	return security_syslog(type);
kernel/printk/printk.c-}
kernel/printk/printk.c-
kernel/printk/printk.c-static void append_char(char **pp, char *e, char c)
```

---

# security hook: security_task_alloc

kernel/fork.c:1

```shell
kernel/fork.c-		goto bad_fork_cleanup_perf;
kernel/fork.c-	/* copy all the process information */
kernel/fork.c-	shm_init_task(p);
kernel/fork.c:	retval = security_task_alloc(p, clone_flags);
kernel/fork.c-	if (retval)
kernel/fork.c-		goto bad_fork_cleanup_audit;
kernel/fork.c-	retval = copy_semundo(clone_flags, p);
```

---

# security hook: security_task_fix_setgid

kernel/sys.c:4

```shell
kernel/sys.c-		new->sgid = new->egid;
kernel/sys.c-	new->fsgid = new->egid;
kernel/sys.c-
kernel/sys.c:	retval = security_task_fix_setgid(new, old, LSM_SETID_RE);
kernel/sys.c-	if (retval < 0)
kernel/sys.c-		goto error;
kernel/sys.c-
--
kernel/sys.c-	else
kernel/sys.c-		goto error;
kernel/sys.c-
kernel/sys.c:	retval = security_task_fix_setgid(new, old, LSM_SETID_ID);
kernel/sys.c-	if (retval < 0)
kernel/sys.c-		goto error;
kernel/sys.c-
--
kernel/sys.c-		new->sgid = ksgid;
kernel/sys.c-	new->fsgid = new->egid;
kernel/sys.c-
kernel/sys.c:	retval = security_task_fix_setgid(new, old, LSM_SETID_RES);
kernel/sys.c-	if (retval < 0)
kernel/sys.c-		goto error;
kernel/sys.c-
--
kernel/sys.c-	    ns_capable_setid(old->user_ns, CAP_SETGID)) {
kernel/sys.c-		if (!gid_eq(kgid, old->fsgid)) {
kernel/sys.c-			new->fsgid = kgid;
kernel/sys.c:			if (security_task_fix_setgid(new,old,LSM_SETID_FS) == 0)
kernel/sys.c-				goto change_okay;
kernel/sys.c-		}
kernel/sys.c-	}
```

---

# security hook: security_task_fix_setgroups

kernel/groups.c:1

```shell
kernel/groups.c-
kernel/groups.c-	set_groups(new, group_info);
kernel/groups.c-
kernel/groups.c:	retval = security_task_fix_setgroups(new, old);
kernel/groups.c-	if (retval < 0)
kernel/groups.c-		goto error;
kernel/groups.c-
```

---

# security hook: security_task_fix_setuid

kernel/sys.c:4
Documentation/admin-guide/LSM/SafeSetID.rst:1

```shell
kernel/sys.c-		new->suid = new->euid;
kernel/sys.c-	new->fsuid = new->euid;
kernel/sys.c-
kernel/sys.c:	retval = security_task_fix_setuid(new, old, LSM_SETID_RE);
kernel/sys.c-	if (retval < 0)
kernel/sys.c-		goto error;
kernel/sys.c-
--
kernel/sys.c-
kernel/sys.c-	new->fsuid = new->euid = kuid;
kernel/sys.c-
kernel/sys.c:	retval = security_task_fix_setuid(new, old, LSM_SETID_ID);
kernel/sys.c-	if (retval < 0)
kernel/sys.c-		goto error;
kernel/sys.c-
--
kernel/sys.c-		new->suid = ksuid;
kernel/sys.c-	new->fsuid = new->euid;
kernel/sys.c-
kernel/sys.c:	retval = security_task_fix_setuid(new, old, LSM_SETID_RES);
kernel/sys.c-	if (retval < 0)
kernel/sys.c-		goto error;
kernel/sys.c-
--
kernel/sys.c-	    ns_capable_setid(old->user_ns, CAP_SETUID)) {
kernel/sys.c-		if (!uid_eq(kuid, old->fsuid)) {
kernel/sys.c-			new->fsuid = kuid;
kernel/sys.c:			if (security_task_fix_setuid(new, old, LSM_SETID_FS) == 0)
kernel/sys.c-				goto change_okay;
kernel/sys.c-		}
kernel/sys.c-	}
--
Documentation/admin-guide/LSM/SafeSetID.rst-Use an existing LSM
Documentation/admin-guide/LSM/SafeSetID.rst--------------------
Documentation/admin-guide/LSM/SafeSetID.rst-None of the other in-tree LSMs have the capability to gate setid transitions, or
Documentation/admin-guide/LSM/SafeSetID.rst:even employ the security_task_fix_setuid hook at all. SELinux says of that hook:
Documentation/admin-guide/LSM/SafeSetID.rst-"Since setuid only affects the current process, and since the SELinux controls
Documentation/admin-guide/LSM/SafeSetID.rst-are not based on the Linux identity attributes, SELinux does not need to control
Documentation/admin-guide/LSM/SafeSetID.rst-this operation."
```

---

# security hook: security_task_free

kernel/fork.c:2

```shell
kernel/fork.c-	io_uring_free(tsk);
kernel/fork.c-	cgroup_free(tsk);
kernel/fork.c-	task_numa_free(tsk, true);
kernel/fork.c:	security_task_free(tsk);
kernel/fork.c-	exit_creds(tsk);
kernel/fork.c-	delayacct_tsk_free(tsk);
kernel/fork.c-	put_signal_struct(tsk->signal);
--
kernel/fork.c-bad_fork_cleanup_semundo:
kernel/fork.c-	exit_sem(p);
kernel/fork.c-bad_fork_cleanup_security:
kernel/fork.c:	security_task_free(p);
kernel/fork.c-bad_fork_cleanup_audit:
kernel/fork.c-	audit_free(p);
kernel/fork.c-bad_fork_cleanup_perf:
```

---

# security hook: security_task_getioprio

block/ioprio.c:2

```shell
block/ioprio.c-{
block/ioprio.c-	int ret;
block/ioprio.c-
block/ioprio.c:	ret = security_task_getioprio(p);
block/ioprio.c-	if (ret)
block/ioprio.c-		goto out;
block/ioprio.c-	task_lock(p);
--
block/ioprio.c-{
block/ioprio.c-	int ret;
block/ioprio.c-
block/ioprio.c:	ret = security_task_getioprio(p);
block/ioprio.c-	if (ret)
block/ioprio.c-		goto out;
block/ioprio.c-	task_lock(p);
```

---

# security hook: security_task_getpgid

kernel/sys.c:1

```shell
kernel/sys.c-		if (!grp)
kernel/sys.c-			goto out;
kernel/sys.c-
kernel/sys.c:		retval = security_task_getpgid(p);
kernel/sys.c-		if (retval)
kernel/sys.c-			goto out;
kernel/sys.c-	}
```

---

# security hook: security_task_getscheduler

kernel/sched/core.c:5
arch/mips/kernel/mips-mt-fpaff.c:1
fs/proc/base.c:1

```shell
arch/mips/kernel/mips-mt-fpaff.c-	p = find_process_by_pid(pid);
arch/mips/kernel/mips-mt-fpaff.c-	if (!p)
arch/mips/kernel/mips-mt-fpaff.c-		goto out_unlock;
arch/mips/kernel/mips-mt-fpaff.c:	retval = security_task_getscheduler(p);
arch/mips/kernel/mips-mt-fpaff.c-	if (retval)
arch/mips/kernel/mips-mt-fpaff.c-		goto out_unlock;
arch/mips/kernel/mips-mt-fpaff.c-
--
kernel/sched/core.c-	rcu_read_lock();
kernel/sched/core.c-	p = find_process_by_pid(pid);
kernel/sched/core.c-	if (p) {
kernel/sched/core.c:		retval = security_task_getscheduler(p);
kernel/sched/core.c-		if (!retval)
kernel/sched/core.c-			retval = p->policy
kernel/sched/core.c-				| (p->sched_reset_on_fork ? SCHED_RESET_ON_FORK : 0);
--
kernel/sched/core.c-	if (!p)
kernel/sched/core.c-		goto out_unlock;
kernel/sched/core.c-
kernel/sched/core.c:	retval = security_task_getscheduler(p);
kernel/sched/core.c-	if (retval)
kernel/sched/core.c-		goto out_unlock;
kernel/sched/core.c-
--
kernel/sched/core.c-	if (!p)
kernel/sched/core.c-		goto out_unlock;
kernel/sched/core.c-
kernel/sched/core.c:	retval = security_task_getscheduler(p);
kernel/sched/core.c-	if (retval)
kernel/sched/core.c-		goto out_unlock;
kernel/sched/core.c-
--
kernel/sched/core.c-	if (!p)
kernel/sched/core.c-		goto out_unlock;
kernel/sched/core.c-
kernel/sched/core.c:	retval = security_task_getscheduler(p);
kernel/sched/core.c-	if (retval)
kernel/sched/core.c-		goto out_unlock;
kernel/sched/core.c-
--
kernel/sched/core.c-	if (!p)
kernel/sched/core.c-		goto out_unlock;
kernel/sched/core.c-
kernel/sched/core.c:	retval = security_task_getscheduler(p);
kernel/sched/core.c-	if (retval)
kernel/sched/core.c-		goto out_unlock;
kernel/sched/core.c-
--
fs/proc/base.c-		}
fs/proc/base.c-		rcu_read_unlock();
fs/proc/base.c-
fs/proc/base.c:		err = security_task_getscheduler(p);
fs/proc/base.c-		if (err)
fs/proc/base.c-			goto out;
fs/proc/base.c-	}
```

---

# security hook: security_task_getsecid_obj

kernel/auditsc.c:3

```shell
kernel/auditsc.c-	context->target_auid = audit_get_loginuid(t);
kernel/auditsc.c-	context->target_uid = task_uid(t);
kernel/auditsc.c-	context->target_sessionid = audit_get_sessionid(t);
kernel/auditsc.c:	security_task_getsecid_obj(t, &context->target_sid);
kernel/auditsc.c-	memcpy(context->target_comm, t->comm, TASK_COMM_LEN);
kernel/auditsc.c-}
kernel/auditsc.c-
--
kernel/auditsc.c-		ctx->target_auid = audit_get_loginuid(t);
kernel/auditsc.c-		ctx->target_uid = t_uid;
kernel/auditsc.c-		ctx->target_sessionid = audit_get_sessionid(t);
kernel/auditsc.c:		security_task_getsecid_obj(t, &ctx->target_sid);
kernel/auditsc.c-		memcpy(ctx->target_comm, t->comm, TASK_COMM_LEN);
kernel/auditsc.c-		return 0;
kernel/auditsc.c-	}
--
kernel/auditsc.c-	axp->target_auid[axp->pid_count] = audit_get_loginuid(t);
kernel/auditsc.c-	axp->target_uid[axp->pid_count] = t_uid;
kernel/auditsc.c-	axp->target_sessionid[axp->pid_count] = audit_get_sessionid(t);
kernel/auditsc.c:	security_task_getsecid_obj(t, &axp->target_sid[axp->pid_count]);
kernel/auditsc.c-	memcpy(axp->target_comm[axp->pid_count], t->comm, TASK_COMM_LEN);
kernel/auditsc.c-	axp->pid_count++;
kernel/auditsc.c-
```

---

# security hook: security_task_getsid

kernel/sys.c:1

```shell
kernel/sys.c-		if (!sid)
kernel/sys.c-			goto out;
kernel/sys.c-
kernel/sys.c:		retval = security_task_getsid(p);
kernel/sys.c-		if (retval)
kernel/sys.c-			goto out;
kernel/sys.c-	}
```

---

# security hook: security_task_kill

kernel/signal.c:2

```shell
kernel/signal.c-		}
kernel/signal.c-	}
kernel/signal.c-
kernel/signal.c:	return security_task_kill(t, info, sig, NULL);
kernel/signal.c-}
kernel/signal.c-
kernel/signal.c-/**
--
kernel/signal.c-		ret = -EPERM;
kernel/signal.c-		goto out_unlock;
kernel/signal.c-	}
kernel/signal.c:	ret = security_task_kill(p, &info, sig, cred);
kernel/signal.c-	if (ret)
kernel/signal.c-		goto out_unlock;
kernel/signal.c-
```

---

# security hook: security_task_movememory

mm/mempolicy.c:1
mm/migrate.c:1

```shell
mm/mempolicy.c-	if (nodes_empty(*new))
mm/mempolicy.c-		goto out_put;
mm/mempolicy.c-
mm/mempolicy.c:	err = security_task_movememory(task);
mm/mempolicy.c-	if (err)
mm/mempolicy.c-		goto out_put;
mm/mempolicy.c-
--
mm/migrate.c-	}
mm/migrate.c-	rcu_read_unlock();
mm/migrate.c-
mm/migrate.c:	mm = ERR_PTR(security_task_movememory(task));
mm/migrate.c-	if (IS_ERR(mm))
mm/migrate.c-		goto out;
mm/migrate.c-	*mem_nodes = cpuset_mems_allowed(task);
```

---

# security hook: security_task_prctl

kernel/sys.c:1

```shell
kernel/sys.c-	unsigned char comm[sizeof(me->comm)];
kernel/sys.c-	long error;
kernel/sys.c-
kernel/sys.c:	error = security_task_prctl(option, arg2, arg3, arg4, arg5);
kernel/sys.c-	if (error != -ENOSYS)
kernel/sys.c-		return error;
kernel/sys.c-
```

---

# security hook: security_task_prlimit

kernel/sys.c:1

```shell
kernel/sys.c-	if (!id_match && !ns_capable(tcred->user_ns, CAP_SYS_RESOURCE))
kernel/sys.c-		return -EPERM;
kernel/sys.c-
kernel/sys.c:	return security_task_prlimit(cred, tcred, flags);
kernel/sys.c-}
kernel/sys.c-
kernel/sys.c-SYSCALL_DEFINE4(prlimit64, pid_t, pid, unsigned int, resource,
```

---

# security hook: security_task_setioprio

block/blk-ioc.c:1

```shell
block/blk-ioc.c-	}
block/blk-ioc.c-	rcu_read_unlock();
block/blk-ioc.c-
block/blk-ioc.c:	err = security_task_setioprio(task, ioprio);
block/blk-ioc.c-	if (err)
block/blk-ioc.c-		return err;
block/blk-ioc.c-
```

---

# security hook: security_task_setnice

kernel/sys.c:1
kernel/sched/core.c:1
kernel/sched/autogroup.c:1

```shell
kernel/sys.c-		error = -EACCES;
kernel/sys.c-		goto out;
kernel/sys.c-	}
kernel/sys.c:	no_nice = security_task_setnice(p, niceval);
kernel/sys.c-	if (no_nice) {
kernel/sys.c-		error = no_nice;
kernel/sys.c-		goto out;
--
kernel/sched/core.c-	if (increment < 0 && !can_nice(current, nice))
kernel/sched/core.c-		return -EPERM;
kernel/sched/core.c-
kernel/sched/core.c:	retval = security_task_setnice(current, nice);
kernel/sched/core.c-	if (retval)
kernel/sched/core.c-		return retval;
kernel/sched/core.c-
--
kernel/sched/autogroup.c-	if (nice < MIN_NICE || nice > MAX_NICE)
kernel/sched/autogroup.c-		return -EINVAL;
kernel/sched/autogroup.c-
kernel/sched/autogroup.c:	err = security_task_setnice(current, nice);
kernel/sched/autogroup.c-	if (err)
kernel/sched/autogroup.c-		return err;
kernel/sched/autogroup.c-
```

---

# security hook: security_task_setpgid

kernel/sys.c:1

```shell
kernel/sys.c-			goto out;
kernel/sys.c-	}
kernel/sys.c-
kernel/sys.c:	err = security_task_setpgid(p, pgid);
kernel/sys.c-	if (err)
kernel/sys.c-		goto out;
kernel/sys.c-
```

---

# security hook: security_task_setrlimit

kernel/sys.c:1

```shell
kernel/sys.c-				!capable(CAP_SYS_RESOURCE))
kernel/sys.c-			retval = -EPERM;
kernel/sys.c-		if (!retval)
kernel/sys.c:			retval = security_task_setrlimit(tsk, resource, new_rlim);
kernel/sys.c-	}
kernel/sys.c-	if (!retval) {
kernel/sys.c-		if (old_rlim)
```

---

# security hook: security_task_setscheduler

kernel/sched/core.c:2
kernel/cgroup/cpuset.c:2
arch/mips/kernel/mips-mt-fpaff.c:1
fs/proc/base.c:1
tools/testing/selftests/cgroup/test_cpuset.c:1

```shell
kernel/cgroup/cpuset.c-		 */
kernel/cgroup/cpuset.c-		if (!cgroup_subsys_on_dfl(cpuset_cgrp_subsys) ||
kernel/cgroup/cpuset.c-		    (cpus_updated || mems_updated)) {
kernel/cgroup/cpuset.c:			ret = security_task_setscheduler(task);
kernel/cgroup/cpuset.c-			if (ret)
kernel/cgroup/cpuset.c-				goto out_unlock;
kernel/cgroup/cpuset.c-		}
--
kernel/cgroup/cpuset.c-	if (ret)
kernel/cgroup/cpuset.c-		goto out_unlock;
kernel/cgroup/cpuset.c-
kernel/cgroup/cpuset.c:	ret = security_task_setscheduler(task);
kernel/cgroup/cpuset.c-	if (ret)
kernel/cgroup/cpuset.c-		goto out_unlock;
kernel/cgroup/cpuset.c-
--
kernel/sched/core.c-		if (attr->sched_flags & SCHED_FLAG_SUGOV)
kernel/sched/core.c-			return -EINVAL;
kernel/sched/core.c-
kernel/sched/core.c:		retval = security_task_setscheduler(p);
kernel/sched/core.c-		if (retval)
kernel/sched/core.c-			return retval;
kernel/sched/core.c-	}
--
kernel/sched/core.c-		rcu_read_unlock();
kernel/sched/core.c-	}
kernel/sched/core.c-
kernel/sched/core.c:	retval = security_task_setscheduler(p);
kernel/sched/core.c-	if (retval)
kernel/sched/core.c-		goto out_put_task;
kernel/sched/core.c-
--
arch/mips/kernel/mips-mt-fpaff.c-		goto out_unlock;
arch/mips/kernel/mips-mt-fpaff.c-	}
arch/mips/kernel/mips-mt-fpaff.c-
arch/mips/kernel/mips-mt-fpaff.c:	retval = security_task_setscheduler(p);
arch/mips/kernel/mips-mt-fpaff.c-	if (retval)
arch/mips/kernel/mips-mt-fpaff.c-		goto out_unlock;
arch/mips/kernel/mips-mt-fpaff.c-
--
fs/proc/base.c-		}
fs/proc/base.c-		rcu_read_unlock();
fs/proc/base.c-
fs/proc/base.c:		err = security_task_setscheduler(p);
fs/proc/base.c-		if (err) {
fs/proc/base.c-			count = err;
fs/proc/base.c-			goto out;
--
tools/testing/selftests/cgroup/test_cpuset.c- * Migrate a process between two sibling cgroups.
tools/testing/selftests/cgroup/test_cpuset.c- * The success should only depend on the parent cgroup permissions and not the
tools/testing/selftests/cgroup/test_cpuset.c- * migrated process itself (cpuset controller is in place because it uses
tools/testing/selftests/cgroup/test_cpuset.c: * security_task_setscheduler() in cgroup v1).
tools/testing/selftests/cgroup/test_cpuset.c- *
tools/testing/selftests/cgroup/test_cpuset.c- * Deliberately don't set cpuset.cpus in children to avoid definining migration
tools/testing/selftests/cgroup/test_cpuset.c- * permissions between two different cpusets.
```

---

# security hook: security_task_to_inode

fs/proc/base.c:3
fs/proc/fd.c:1

```shell
fs/proc/base.c-	ei->pid = pid;
fs/proc/base.c-
fs/proc/base.c-	task_dump_owner(task, 0, &inode->i_uid, &inode->i_gid);
fs/proc/base.c:	security_task_to_inode(task, inode);
fs/proc/base.c-
fs/proc/base.c-out:
fs/proc/base.c-	return inode;
--
fs/proc/base.c-	task_dump_owner(task, inode->i_mode, &inode->i_uid, &inode->i_gid);
fs/proc/base.c-
fs/proc/base.c-	inode->i_mode &= ~(S_ISUID | S_ISGID);
fs/proc/base.c:	security_task_to_inode(task, inode);
fs/proc/base.c-}
fs/proc/base.c-
fs/proc/base.c-/*
--
fs/proc/base.c-	if (exact_vma_exists) {
fs/proc/base.c-		task_dump_owner(task, 0, &inode->i_uid, &inode->i_gid);
fs/proc/base.c-
fs/proc/base.c:		security_task_to_inode(task, inode);
fs/proc/base.c-		status = 1;
fs/proc/base.c-	}
fs/proc/base.c-
--
fs/proc/fd.c-			i_mode |= S_IWUSR | S_IXUSR;
fs/proc/fd.c-		inode->i_mode = i_mode;
fs/proc/fd.c-	}
fs/proc/fd.c:	security_task_to_inode(task, inode);
fs/proc/fd.c-}
fs/proc/fd.c-
fs/proc/fd.c-static int tid_fd_revalidate(struct dentry *dentry, unsigned int flags)
```

---

# security hook: security_transfer_creds


```shell
```

---

# security hook: security_tun_dev_alloc_security

drivers/net/tun.c:1

```shell
drivers/net/tun.c-
drivers/net/tun.c-	spin_lock_init(&tun->lock);
drivers/net/tun.c-
drivers/net/tun.c:	err = security_tun_dev_alloc_security(&tun->security);
drivers/net/tun.c-	if (err < 0) {
drivers/net/tun.c-		free_percpu(dev->tstats);
drivers/net/tun.c-		return err;
```

---

# security hook: security_tun_dev_attach

drivers/net/tun.c:2

```shell
drivers/net/tun.c-	struct net_device *dev = tun->dev;
drivers/net/tun.c-	int err;
drivers/net/tun.c-
drivers/net/tun.c:	err = security_tun_dev_attach(tfile->socket.sk, tun->security);
drivers/net/tun.c-	if (err < 0)
drivers/net/tun.c-		goto out;
drivers/net/tun.c-
--
drivers/net/tun.c-			ret = -EINVAL;
drivers/net/tun.c-			goto unlock;
drivers/net/tun.c-		}
drivers/net/tun.c:		ret = security_tun_dev_attach_queue(tun->security);
drivers/net/tun.c-		if (ret < 0)
drivers/net/tun.c-			goto unlock;
drivers/net/tun.c-		ret = tun_attach(tun, file, false, tun->flags & IFF_NAPI,
```

---

# security hook: security_tun_dev_attach_queue

drivers/net/tun.c:1

```shell
drivers/net/tun.c-			ret = -EINVAL;
drivers/net/tun.c-			goto unlock;
drivers/net/tun.c-		}
drivers/net/tun.c:		ret = security_tun_dev_attach_queue(tun->security);
drivers/net/tun.c-		if (ret < 0)
drivers/net/tun.c-			goto unlock;
drivers/net/tun.c-		ret = tun_attach(tun, file, false, tun->flags & IFF_NAPI,
```

---

# security hook: security_tun_dev_create

drivers/net/tun.c:1

```shell
drivers/net/tun.c-
drivers/net/tun.c-		if (!ns_capable(net->user_ns, CAP_NET_ADMIN))
drivers/net/tun.c-			return -EPERM;
drivers/net/tun.c:		err = security_tun_dev_create();
drivers/net/tun.c-		if (err < 0)
drivers/net/tun.c-			return err;
drivers/net/tun.c-
```

---

# security hook: security_tun_dev_free_security

drivers/net/tun.c:2

```shell
drivers/net/tun.c-			 ifr->ifr_flags & IFF_NAPI_FRAGS, false);
drivers/net/tun.c-	if (err < 0) {
drivers/net/tun.c-		tun_flow_uninit(tun);
drivers/net/tun.c:		security_tun_dev_free_security(tun->security);
drivers/net/tun.c-		free_percpu(dev->tstats);
drivers/net/tun.c-		return err;
drivers/net/tun.c-	}
--
drivers/net/tun.c-
drivers/net/tun.c-	free_percpu(dev->tstats);
drivers/net/tun.c-	tun_flow_uninit(tun);
drivers/net/tun.c:	security_tun_dev_free_security(tun->security);
drivers/net/tun.c-	__tun_set_ebpf(tun, &tun->steering_prog, NULL);
drivers/net/tun.c-	__tun_set_ebpf(tun, &tun->filter_prog, NULL);
drivers/net/tun.c-}
```

---

# security hook: security_tun_dev_open

drivers/net/tun.c:1

```shell
drivers/net/tun.c-
drivers/net/tun.c-		if (tun_not_capable(tun))
drivers/net/tun.c-			return -EPERM;
drivers/net/tun.c:		err = security_tun_dev_open(tun->security);
drivers/net/tun.c-		if (err < 0)
drivers/net/tun.c-			return err;
drivers/net/tun.c-
```

---

# security hook: security_unix_may_send

net/unix/af_unix.c:2

```shell
net/unix/af_unix.c-		if (!unix_may_send(sk, other))
net/unix/af_unix.c-			goto out_unlock;
net/unix/af_unix.c-
net/unix/af_unix.c:		err = security_unix_may_send(sk->sk_socket, other->sk_socket);
net/unix/af_unix.c-		if (err)
net/unix/af_unix.c-			goto out_unlock;
net/unix/af_unix.c-
--
net/unix/af_unix.c-		goto out_unlock;
net/unix/af_unix.c-
net/unix/af_unix.c-	if (sk->sk_type != SOCK_SEQPACKET) {
net/unix/af_unix.c:		err = security_unix_may_send(sk->sk_socket, other->sk_socket);
net/unix/af_unix.c-		if (err)
net/unix/af_unix.c-			goto out_unlock;
net/unix/af_unix.c-	}
```

---

# security hook: security_unix_stream_connect

net/unix/af_unix.c:1

```shell
net/unix/af_unix.c-		goto out_unlock;
net/unix/af_unix.c-	}
net/unix/af_unix.c-
net/unix/af_unix.c:	err = security_unix_stream_connect(sk, other, newsk);
net/unix/af_unix.c-	if (err) {
net/unix/af_unix.c-		unix_state_unlock(sk);
net/unix/af_unix.c-		goto out_unlock;
```

---

# security hook: security_uring_cmd

io_uring/uring_cmd.c:1

```shell
io_uring/uring_cmd.c-	if (!file->f_op->uring_cmd)
io_uring/uring_cmd.c-		return -EOPNOTSUPP;
io_uring/uring_cmd.c-
io_uring/uring_cmd.c:	ret = security_uring_cmd(ioucmd);
io_uring/uring_cmd.c-	if (ret)
io_uring/uring_cmd.c-		return ret;
io_uring/uring_cmd.c-
```

---

# security hook: security_uring_override_creds

io_uring/io_uring.c:1

```shell
io_uring/io_uring.c-		if (!req->creds)
io_uring/io_uring.c-			return io_init_fail_req(req, -EINVAL);
io_uring/io_uring.c-		get_cred(req->creds);
io_uring/io_uring.c:		ret = security_uring_override_creds(req->creds);
io_uring/io_uring.c-		if (ret) {
io_uring/io_uring.c-			put_cred(req->creds);
io_uring/io_uring.c-			return io_init_fail_req(req, ret);
```

---

# security hook: security_uring_sqpoll

io_uring/sqpoll.c:1

```shell
io_uring/sqpoll.c-		struct io_sq_data *sqd;
io_uring/sqpoll.c-		bool attached;
io_uring/sqpoll.c-
io_uring/sqpoll.c:		ret = security_uring_sqpoll();
io_uring/sqpoll.c-		if (ret)
io_uring/sqpoll.c-			return ret;
io_uring/sqpoll.c-
```

---

# security hook: security_vm_enough_memory_mm

mm/mprotect.c:1
mm/mmap.c:4
mm/mremap.c:2
mm/shmem.c:3
mm/swapfile.c:1
kernel/fork.c:1

```shell
mm/mprotect.c-		if (!(oldflags & (VM_ACCOUNT|VM_WRITE|VM_HUGETLB|
mm/mprotect.c-						VM_SHARED|VM_NORESERVE))) {
mm/mprotect.c-			charged = nrpages;
mm/mprotect.c:			if (security_vm_enough_memory_mm(mm, charged))
mm/mprotect.c-				return -ENOMEM;
mm/mprotect.c-			newflags |= VM_ACCOUNT;
mm/mprotect.c-		}
--
mm/mmap.c-	 * Overcommit..  This must be the final test, as it will
mm/mmap.c-	 * update security statistics.
mm/mmap.c-	 */
mm/mmap.c:	if (security_vm_enough_memory_mm(mm, grow))
mm/mmap.c-		return -ENOMEM;
mm/mmap.c-
mm/mmap.c-	return 0;
--
mm/mmap.c-	 */
mm/mmap.c-	if (accountable_mapping(file, vm_flags)) {
mm/mmap.c-		charged = len >> PAGE_SHIFT;
mm/mmap.c:		if (security_vm_enough_memory_mm(mm, charged))
mm/mmap.c-			return -ENOMEM;
mm/mmap.c-		vm_flags |= VM_ACCOUNT;
mm/mmap.c-	}
--
mm/mmap.c-	if (mm->map_count > sysctl_max_map_count)
mm/mmap.c-		return -ENOMEM;
mm/mmap.c-
mm/mmap.c:	if (security_vm_enough_memory_mm(mm, len >> PAGE_SHIFT))
mm/mmap.c-		return -ENOMEM;
mm/mmap.c-
mm/mmap.c-	/*
--
mm/mmap.c-		return -ENOMEM;
mm/mmap.c-
mm/mmap.c-	if ((vma->vm_flags & VM_ACCOUNT) &&
mm/mmap.c:	     security_vm_enough_memory_mm(mm, charged))
mm/mmap.c-		return -ENOMEM;
mm/mmap.c-
mm/mmap.c-	/*
--
mm/mremap.c-		return err;
mm/mremap.c-
mm/mremap.c-	if (vm_flags & VM_ACCOUNT) {
mm/mremap.c:		if (security_vm_enough_memory_mm(mm, to_account >> PAGE_SHIFT))
mm/mremap.c-			return -ENOMEM;
mm/mremap.c-	}
mm/mremap.c-
--
mm/mremap.c-			VMA_ITERATOR(vmi, mm, extension_start);
mm/mremap.c-
mm/mremap.c-			if (vma->vm_flags & VM_ACCOUNT) {
mm/mremap.c:				if (security_vm_enough_memory_mm(mm, pages)) {
mm/mremap.c-					ret = -ENOMEM;
mm/mremap.c-					goto out;
mm/mremap.c-				}
--
mm/shmem.c-static inline int shmem_acct_size(unsigned long flags, loff_t size)
mm/shmem.c-{
mm/shmem.c-	return (flags & VM_NORESERVE) ?
mm/shmem.c:		0 : security_vm_enough_memory_mm(current->mm, VM_ACCT(size));
mm/shmem.c-}
mm/shmem.c-
mm/shmem.c-static inline void shmem_unacct_size(unsigned long flags, loff_t size)
--
mm/shmem.c-{
mm/shmem.c-	if (!(flags & VM_NORESERVE)) {
mm/shmem.c-		if (VM_ACCT(newsize) > VM_ACCT(oldsize))
mm/shmem.c:			return security_vm_enough_memory_mm(current->mm,
mm/shmem.c-					VM_ACCT(newsize) - VM_ACCT(oldsize));
mm/shmem.c-		else if (VM_ACCT(newsize) < VM_ACCT(oldsize))
mm/shmem.c-			vm_unacct_memory(VM_ACCT(oldsize) - VM_ACCT(newsize));
--
mm/shmem.c-	if (!(flags & VM_NORESERVE))
mm/shmem.c-		return 0;
mm/shmem.c-
mm/shmem.c:	return security_vm_enough_memory_mm(current->mm,
mm/shmem.c-			pages * VM_ACCT(PAGE_SIZE));
mm/shmem.c-}
mm/shmem.c-
--
mm/swapfile.c-		spin_unlock(&swap_lock);
mm/swapfile.c-		goto out_dput;
mm/swapfile.c-	}
mm/swapfile.c:	if (!security_vm_enough_memory_mm(current->mm, p->pages))
mm/swapfile.c-		vm_unacct_memory(p->pages);
mm/swapfile.c-	else {
mm/swapfile.c-		err = -ENOMEM;
--
kernel/fork.c-		if (mpnt->vm_flags & VM_ACCOUNT) {
kernel/fork.c-			unsigned long len = vma_pages(mpnt);
kernel/fork.c-
kernel/fork.c:			if (security_vm_enough_memory_mm(oldmm, len)) /* sic */
kernel/fork.c-				goto fail_nomem;
kernel/fork.c-			charge = len;
kernel/fork.c-		}
```

---

# security hook: security_watch_key


```shell
```

---

# security hook: security_xfrm_decode_session

net/xfrm/xfrm_policy.c:1

```shell
net/xfrm/xfrm_policy.c-		fl->flowi_oif = oif;
net/xfrm/xfrm_policy.c-	}
net/xfrm/xfrm_policy.c-
net/xfrm/xfrm_policy.c:	return security_xfrm_decode_session(skb, &fl->flowi_secid);
net/xfrm/xfrm_policy.c-}
net/xfrm/xfrm_policy.c-EXPORT_SYMBOL(__xfrm_decode_session);
net/xfrm/xfrm_policy.c-
```

---

# security hook: security_xfrm_policy_alloc

net/xfrm/xfrm_user.c:3
net/key/af_key.c:3

```shell
net/xfrm/xfrm_user.c-		return 0;
net/xfrm/xfrm_user.c-
net/xfrm/xfrm_user.c-	uctx = nla_data(rt);
net/xfrm/xfrm_user.c:	return security_xfrm_policy_alloc(&pol->security, uctx, GFP_KERNEL);
net/xfrm/xfrm_user.c-}
net/xfrm/xfrm_user.c-
net/xfrm/xfrm_user.c-static void copy_templates(struct xfrm_policy *xp, struct xfrm_user_tmpl *ut,
--
net/xfrm/xfrm_user.c-		if (rt) {
net/xfrm/xfrm_user.c-			struct xfrm_user_sec_ctx *uctx = nla_data(rt);
net/xfrm/xfrm_user.c-
net/xfrm/xfrm_user.c:			err = security_xfrm_policy_alloc(&ctx, uctx, GFP_KERNEL);
net/xfrm/xfrm_user.c-			if (err)
net/xfrm/xfrm_user.c-				return err;
net/xfrm/xfrm_user.c-		}
--
net/xfrm/xfrm_user.c-		if (rt) {
net/xfrm/xfrm_user.c-			struct xfrm_user_sec_ctx *uctx = nla_data(rt);
net/xfrm/xfrm_user.c-
net/xfrm/xfrm_user.c:			err = security_xfrm_policy_alloc(&ctx, uctx, GFP_KERNEL);
net/xfrm/xfrm_user.c-			if (err)
net/xfrm/xfrm_user.c-				return err;
net/xfrm/xfrm_user.c-		}
--
net/key/af_key.c-			goto out;
net/key/af_key.c-		}
net/key/af_key.c-
net/key/af_key.c:		err = security_xfrm_policy_alloc(&xp->security, uctx, GFP_KERNEL);
net/key/af_key.c-		kfree(uctx);
net/key/af_key.c-
net/key/af_key.c-		if (err)
--
net/key/af_key.c-		if (!uctx)
net/key/af_key.c-			return -ENOMEM;
net/key/af_key.c-
net/key/af_key.c:		err = security_xfrm_policy_alloc(&pol_ctx, uctx, GFP_KERNEL);
net/key/af_key.c-		kfree(uctx);
net/key/af_key.c-		if (err)
net/key/af_key.c-			return err;
--
net/key/af_key.c-		if ((*dir = verify_sec_ctx_len(p)))
net/key/af_key.c-			goto out;
net/key/af_key.c-		uctx = pfkey_sadb2xfrm_user_sec_ctx(sec_ctx, GFP_ATOMIC);
net/key/af_key.c:		*dir = security_xfrm_policy_alloc(&xp->security, uctx, GFP_ATOMIC);
net/key/af_key.c-		kfree(uctx);
net/key/af_key.c-
net/key/af_key.c-		if (*dir)
```

---

# security hook: security_xfrm_policy_clone

net/xfrm/xfrm_policy.c:1

```shell
net/xfrm/xfrm_policy.c-
net/xfrm/xfrm_policy.c-	if (newp) {
net/xfrm/xfrm_policy.c-		newp->selector = old->selector;
net/xfrm/xfrm_policy.c:		if (security_xfrm_policy_clone(old->security,
net/xfrm/xfrm_policy.c-					       &newp->security)) {
net/xfrm/xfrm_policy.c-			kfree(newp);
net/xfrm/xfrm_policy.c-			return NULL;  /* ENOMEM */
```

---

# security hook: security_xfrm_policy_delete

net/xfrm/xfrm_policy.c:4

```shell
net/xfrm/xfrm_policy.c-	if (pol) {
net/xfrm/xfrm_policy.c-		xfrm_pol_hold(pol);
net/xfrm/xfrm_policy.c-		if (delete) {
net/xfrm/xfrm_policy.c:			*err = security_xfrm_policy_delete(pol->security);
net/xfrm/xfrm_policy.c-			if (*err) {
net/xfrm/xfrm_policy.c-				spin_unlock_bh(&net->xfrm.xfrm_policy_lock);
net/xfrm/xfrm_policy.c-				return pol;
--
net/xfrm/xfrm_policy.c-		    pol->if_id == if_id && xfrm_policy_mark_match(mark, pol)) {
net/xfrm/xfrm_policy.c-			xfrm_pol_hold(pol);
net/xfrm/xfrm_policy.c-			if (delete) {
net/xfrm/xfrm_policy.c:				*err = security_xfrm_policy_delete(
net/xfrm/xfrm_policy.c-								pol->security);
net/xfrm/xfrm_policy.c-				if (*err) {
net/xfrm/xfrm_policy.c-					spin_unlock_bh(&net->xfrm.xfrm_policy_lock);
--
net/xfrm/xfrm_policy.c-		    pol->type != type)
net/xfrm/xfrm_policy.c-			continue;
net/xfrm/xfrm_policy.c-
net/xfrm/xfrm_policy.c:		err = security_xfrm_policy_delete(pol->security);
net/xfrm/xfrm_policy.c-		if (err) {
net/xfrm/xfrm_policy.c-			xfrm_audit_policy_delete(pol, 0, task_valid);
net/xfrm/xfrm_policy.c-			return err;
--
net/xfrm/xfrm_policy.c-		    pol->xdo.dev != dev)
net/xfrm/xfrm_policy.c-			continue;
net/xfrm/xfrm_policy.c-
net/xfrm/xfrm_policy.c:		err = security_xfrm_policy_delete(pol->security);
net/xfrm/xfrm_policy.c-		if (err) {
net/xfrm/xfrm_policy.c-			xfrm_audit_policy_delete(pol, 0, task_valid);
net/xfrm/xfrm_policy.c-			return err;
```

---

# security hook: security_xfrm_policy_free

net/xfrm/xfrm_user.c:3
net/xfrm/xfrm_policy.c:1
net/key/af_key.c:1

```shell
net/xfrm/xfrm_user.c-	if (err) {
net/xfrm/xfrm_user.c-		xfrm_dev_policy_delete(xp);
net/xfrm/xfrm_user.c-		xfrm_dev_policy_free(xp);
net/xfrm/xfrm_user.c:		security_xfrm_policy_free(xp->security);
net/xfrm/xfrm_user.c-		kfree(xp);
net/xfrm/xfrm_user.c-		return err;
net/xfrm/xfrm_user.c-	}
--
net/xfrm/xfrm_user.c-		}
net/xfrm/xfrm_user.c-		xp = xfrm_policy_bysel_ctx(net, &m, if_id, type, p->dir,
net/xfrm/xfrm_user.c-					   &p->sel, ctx, delete, &err);
net/xfrm/xfrm_user.c:		security_xfrm_policy_free(ctx);
net/xfrm/xfrm_user.c-	}
net/xfrm/xfrm_user.c-	if (xp == NULL)
net/xfrm/xfrm_user.c-		return -ENOENT;
--
net/xfrm/xfrm_user.c-		}
net/xfrm/xfrm_user.c-		xp = xfrm_policy_bysel_ctx(net, &m, if_id, type, p->dir,
net/xfrm/xfrm_user.c-					   &p->sel, ctx, 0, &err);
net/xfrm/xfrm_user.c:		security_xfrm_policy_free(ctx);
net/xfrm/xfrm_user.c-	}
net/xfrm/xfrm_user.c-	if (xp == NULL)
net/xfrm/xfrm_user.c-		return -ENOENT;
--
net/xfrm/xfrm_policy.c-{
net/xfrm/xfrm_policy.c-	struct xfrm_policy *policy = container_of(head, struct xfrm_policy, rcu);
net/xfrm/xfrm_policy.c-
net/xfrm/xfrm_policy.c:	security_xfrm_policy_free(policy->security);
net/xfrm/xfrm_policy.c-	kfree(policy);
net/xfrm/xfrm_policy.c-}
net/xfrm/xfrm_policy.c-
--
net/key/af_key.c-	xp = xfrm_policy_bysel_ctx(net, &dummy_mark, 0, XFRM_POLICY_TYPE_MAIN,
net/key/af_key.c-				   pol->sadb_x_policy_dir - 1, &sel, pol_ctx,
net/key/af_key.c-				   1, &err);
net/key/af_key.c:	security_xfrm_policy_free(pol_ctx);
net/key/af_key.c-	if (xp == NULL)
net/key/af_key.c-		return -ENOENT;
net/key/af_key.c-
```

---

# security hook: security_xfrm_policy_lookup

net/xfrm/xfrm_policy.c:2

```shell
net/xfrm/xfrm_policy.c-
net/xfrm/xfrm_policy.c-	match = xfrm_selector_match(sel, fl, family);
net/xfrm/xfrm_policy.c-	if (match)
net/xfrm/xfrm_policy.c:		ret = security_xfrm_policy_lookup(pol->security, fl->flowi_secid);
net/xfrm/xfrm_policy.c-	return ret;
net/xfrm/xfrm_policy.c-}
net/xfrm/xfrm_policy.c-
--
net/xfrm/xfrm_policy.c-				pol = NULL;
net/xfrm/xfrm_policy.c-				goto out;
net/xfrm/xfrm_policy.c-			}
net/xfrm/xfrm_policy.c:			err = security_xfrm_policy_lookup(pol->security,
net/xfrm/xfrm_policy.c-						      fl->flowi_secid);
net/xfrm/xfrm_policy.c-			if (!err) {
net/xfrm/xfrm_policy.c-				if (!xfrm_pol_hold_rcu(pol))
```

---

# security hook: security_xfrm_state_alloc

net/xfrm/xfrm_state.c:2
net/xfrm/xfrm_user.c:1
net/key/af_key.c:1

```shell
net/xfrm/xfrm_state.c-		memcpy(&x->mark, &pol->mark, sizeof(x->mark));
net/xfrm/xfrm_state.c-		x->if_id = if_id;
net/xfrm/xfrm_state.c-
net/xfrm/xfrm_state.c:		error = security_xfrm_state_alloc_acquire(x, pol->security, fl->flowi_secid);
net/xfrm/xfrm_state.c-		if (error) {
net/xfrm/xfrm_state.c-			x->km.state = XFRM_STATE_DEAD;
net/xfrm/xfrm_state.c-			to_put = x;
--
net/xfrm/xfrm_state.c-	uctx->ctx_alg = security->ctx_alg;
net/xfrm/xfrm_state.c-	uctx->ctx_len = security->ctx_len;
net/xfrm/xfrm_state.c-	memcpy(uctx + 1, security->ctx_str, security->ctx_len);
net/xfrm/xfrm_state.c:	err = security_xfrm_state_alloc(x, uctx);
net/xfrm/xfrm_state.c-	kfree(uctx);
net/xfrm/xfrm_state.c-	if (err)
net/xfrm/xfrm_state.c-		return err;
--
net/xfrm/xfrm_user.c-		goto error;
net/xfrm/xfrm_user.c-
net/xfrm/xfrm_user.c-	if (attrs[XFRMA_SEC_CTX]) {
net/xfrm/xfrm_user.c:		err = security_xfrm_state_alloc(x,
net/xfrm/xfrm_user.c-						nla_data(attrs[XFRMA_SEC_CTX]));
net/xfrm/xfrm_user.c-		if (err)
net/xfrm/xfrm_user.c-			goto error;
--
net/key/af_key.c-		if (!uctx)
net/key/af_key.c-			goto out;
net/key/af_key.c-
net/key/af_key.c:		err = security_xfrm_state_alloc(x, uctx);
net/key/af_key.c-		kfree(uctx);
net/key/af_key.c-
net/key/af_key.c-		if (err)
```

---

# security hook: security_xfrm_state_alloc_acquire

net/xfrm/xfrm_state.c:1

```shell
net/xfrm/xfrm_state.c-		memcpy(&x->mark, &pol->mark, sizeof(x->mark));
net/xfrm/xfrm_state.c-		x->if_id = if_id;
net/xfrm/xfrm_state.c-
net/xfrm/xfrm_state.c:		error = security_xfrm_state_alloc_acquire(x, pol->security, fl->flowi_secid);
net/xfrm/xfrm_state.c-		if (error) {
net/xfrm/xfrm_state.c-			x->km.state = XFRM_STATE_DEAD;
net/xfrm/xfrm_state.c-			to_put = x;
```

---

# security hook: security_xfrm_state_delete

net/xfrm/xfrm_state.c:2
net/xfrm/xfrm_user.c:1
net/key/af_key.c:1

```shell
net/xfrm/xfrm_state.c-
net/xfrm/xfrm_state.c-		hlist_for_each_entry(x, net->xfrm.state_bydst+i, bydst) {
net/xfrm/xfrm_state.c-			if (xfrm_id_proto_match(x->id.proto, proto) &&
net/xfrm/xfrm_state.c:			   (err = security_xfrm_state_delete(x)) != 0) {
net/xfrm/xfrm_state.c-				xfrm_audit_state_delete(x, 0, task_valid);
net/xfrm/xfrm_state.c-				return err;
net/xfrm/xfrm_state.c-			}
--
net/xfrm/xfrm_state.c-			xso = &x->xso;
net/xfrm/xfrm_state.c-
net/xfrm/xfrm_state.c-			if (xso->dev == dev &&
net/xfrm/xfrm_state.c:			   (err = security_xfrm_state_delete(x)) != 0) {
net/xfrm/xfrm_state.c-				xfrm_audit_state_delete(x, 0, task_valid);
net/xfrm/xfrm_state.c-				return err;
net/xfrm/xfrm_state.c-			}
--
net/xfrm/xfrm_user.c-	if (x == NULL)
net/xfrm/xfrm_user.c-		return err;
net/xfrm/xfrm_user.c-
net/xfrm/xfrm_user.c:	if ((err = security_xfrm_state_delete(x)) != 0)
net/xfrm/xfrm_user.c-		goto out;
net/xfrm/xfrm_user.c-
net/xfrm/xfrm_user.c-	if (xfrm_state_kern(x)) {
--
net/key/af_key.c-	if (x == NULL)
net/key/af_key.c-		return -ESRCH;
net/key/af_key.c-
net/key/af_key.c:	if ((err = security_xfrm_state_delete(x)))
net/key/af_key.c-		goto out;
net/key/af_key.c-
net/key/af_key.c-	if (xfrm_state_kern(x)) {
```

---

# security hook: security_xfrm_state_free

net/xfrm/xfrm_state.c:1

```shell
net/xfrm/xfrm_state.c-	if (x->xfrag.page)
net/xfrm/xfrm_state.c-		put_page(x->xfrag.page);
net/xfrm/xfrm_state.c-	xfrm_dev_state_free(x);
net/xfrm/xfrm_state.c:	security_xfrm_state_free(x);
net/xfrm/xfrm_state.c-	xfrm_state_free(x);
net/xfrm/xfrm_state.c-}
net/xfrm/xfrm_state.c-
```

---

# security hook: security_xfrm_state_pol_flow_match

net/xfrm/xfrm_state.c:2

```shell
net/xfrm/xfrm_state.c-		if ((x->sel.family &&
net/xfrm/xfrm_state.c-		     (x->sel.family != family ||
net/xfrm/xfrm_state.c-		      !xfrm_selector_match(&x->sel, fl, family))) ||
net/xfrm/xfrm_state.c:		    !security_xfrm_state_pol_flow_match(x, pol,
net/xfrm/xfrm_state.c-							&fl->u.__fl_common))
net/xfrm/xfrm_state.c-			return;
net/xfrm/xfrm_state.c-
--
net/xfrm/xfrm_state.c-		if ((!x->sel.family ||
net/xfrm/xfrm_state.c-		     (x->sel.family == family &&
net/xfrm/xfrm_state.c-		      xfrm_selector_match(&x->sel, fl, family))) &&
net/xfrm/xfrm_state.c:		    security_xfrm_state_pol_flow_match(x, pol,
net/xfrm/xfrm_state.c-						       &fl->u.__fl_common))
net/xfrm/xfrm_state.c-			*error = -ESRCH;
net/xfrm/xfrm_state.c-	}
```

---

