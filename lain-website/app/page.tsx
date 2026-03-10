import styles from "./page.module.css";
import Hero from "../components/Hero";
import FeatureGrid from "../components/FeatureGrid";
import CodeShowcase from "../components/CodeShowcase";
import TerminalWindow from "../components/TerminalWindow";

export default function Home() {
  return (
    <div className={styles.container}>
      <Hero />
      <FeatureGrid />
      <CodeShowcase />
      <TerminalWindow />
    </div>
  );
}
